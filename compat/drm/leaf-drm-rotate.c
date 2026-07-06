/*
 * LD_PRELOAD DRM rotation shim for direct-display apps on portrait-mounted
 * panels, such as MLP1's 720x960 DSI panel in a landscape handheld.
 *
 * Inside the target process this shim:
 *   1. Swaps GETCONNECTOR/GETCRTC mode dimensions so SDL, Mali VK_KHR_display,
 *      and the engine see a landscape display.
 *   2. Tracks app framebuffers at ADDFB/ADDFB2 time and immediately exports
 *      dma-buf fds, before Mali closes and reuses the GEM handles.
 *   3. Rotates each SETCRTC/PAGE_FLIP buffer through RGA into shadow dumb
 *      framebuffers matching the real portrait panel mode.
 *
 * Env:
 *   LEAF_DRM_ROTATE       = 90 | 270   rotation to apply at scanout
 *   LEAF_DRM_ROTATE_DEBUG = 1          log to stderr
 *   LEAF_DRM_ROTATE_DUMP  = /path.raw  dump one rotated frame
 *   LEAF_DRM_ROTATE_DUMP_FRAME=N       delay dump until frame N
 *
 * Nothing persists: all state is per-process and GEM objects are released by
 * the kernel when the process exits.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

#define SHADOW_COUNT 3
#define FB_TABLE_MAX 32

/*
 * Minimal Rockchip RGA userspace ABI subset. librga is opened dynamically at
 * runtime; the struct layout matches the RgaApi.h/drmrga.h linux C interface
 * used by the stock MLP1 librga.so.2.
 */
#define HAL_TRANSFORM_ROT_90 0x04
#define HAL_TRANSFORM_ROT_270 0x07
#define RK_FORMAT_BGRA_8888 (0x3 << 8)

typedef struct leaf_rga_rect {
    int xoffset;
    int yoffset;
    int width;
    int height;
    int wstride;
    int hstride;
    int format;
    int size;
} leaf_rga_rect_t;

typedef struct leaf_rga_nn {
    int nn_flag;
    int scale_r;
    int scale_g;
    int scale_b;
    int offset_r;
    int offset_g;
    int offset_b;
} leaf_rga_nn_t;

typedef struct leaf_rga_dither {
    int enable;
    int mode;
    int lut0_l;
    int lut0_h;
    int lut1_l;
    int lut1_h;
} leaf_rga_dither_t;

struct leaf_rga_mosaic_info {
    uint8_t enable;
    uint8_t mode;
};

struct leaf_rga_gauss_config {
    uint32_t size;
    uint64_t coe_ptr;
};

struct leaf_rga_pre_intr_info {
    uint8_t enable;
    uint8_t read_intr_en;
    uint8_t write_intr_en;
    uint8_t read_hold_en;
    uint32_t read_threshold;
    uint32_t write_start;
    uint32_t write_step;
};

struct leaf_rga_color {
    union {
        struct {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint8_t alpha;
        };
        uint32_t value;
    };
};

struct leaf_rga_osd_invert_factor {
    uint8_t alpha_max;
    uint8_t alpha_min;
    uint8_t yg_max;
    uint8_t yg_min;
    uint8_t crb_max;
    uint8_t crb_min;
};

struct leaf_rga_osd_bpp2 {
    uint8_t ac_swap;
    uint8_t endian_swap;
    struct leaf_rga_color color0;
    struct leaf_rga_color color1;
};

struct leaf_rga_osd_mode_ctrl {
    uint8_t mode;
    uint8_t direction_mode;
    uint8_t width_mode;
    uint16_t block_fix_width;
    uint8_t block_num;
    uint16_t flags_index;
    uint8_t color_mode;
    uint8_t invert_flags_mode;
    uint8_t default_color_sel;
    uint8_t invert_enable;
    uint8_t invert_mode;
    uint8_t invert_thresh;
    uint8_t unfix_index;
};

struct leaf_rga_osd_info {
    uint8_t enable;
    struct leaf_rga_osd_mode_ctrl mode_ctrl;
    struct leaf_rga_osd_invert_factor cal_factor;
    struct leaf_rga_osd_bpp2 bpp2_info;
    union {
        struct {
            uint32_t last_flags1;
            uint32_t last_flags0;
        };
        uint64_t last_flags;
    };
    union {
        struct {
            uint32_t cur_flags1;
            uint32_t cur_flags0;
        };
        uint64_t cur_flags;
    };
};

typedef struct leaf_rga_info {
    int fd;
    void *virAddr;
    void *phyAddr;
    unsigned hnd;
    int format;
    leaf_rga_rect_t rect;
    unsigned int blend;
    int bufferSize;
    int rotation;
    int color;
    int testLog;
    int mmuFlag;
    int colorkey_en;
    int colorkey_mode;
    int colorkey_max;
    int colorkey_min;
    int scale_mode;
    int color_space_mode;
    int sync_mode;
    leaf_rga_nn_t nn;
    leaf_rga_dither_t dither;
    int rop_code;
    int rd_mode;
    unsigned short is_10b_compact;
    unsigned short is_10b_endian;
    int in_fence_fd;
    int out_fence_fd;
    int core;
    int priority;
    unsigned short enable;
    int handle;
    struct leaf_rga_mosaic_info mosaic_info;
    struct leaf_rga_osd_info osd_info;
    struct leaf_rga_pre_intr_info pre_intr;
    int mpi_mode;
    union {
        int ctx_id;
        int job_handle;
    };
    uint16_t rgba5551_flags;
    uint8_t rgba5551_alpha0;
    uint8_t rgba5551_alpha1;
    struct leaf_rga_gauss_config gauss_config;
    char reserve[386];
} leaf_rga_info_t;

static int leaf_rga_set_rect(leaf_rga_rect_t *rect,
                             int x,
                             int y,
                             int w,
                             int h,
                             int sw,
                             int sh,
                             int format)
{
    if (!rect) {
        return -EINVAL;
    }
    rect->xoffset = x;
    rect->yoffset = y;
    rect->width = w;
    rect->height = h;
    rect->wstride = sw;
    rect->hstride = sh;
    rect->format = format;
    return 0;
}

static int (*real_ioctl)(int, unsigned long, ...);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_rotate;
static int g_debug;
static int g_initialized;

static struct drm_mode_modeinfo g_real_mode;
static int g_have_real_mode;

struct tracked_fb {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    int prime_fd;
};
static struct tracked_fb g_fbs[FB_TABLE_MAX];

struct shadow_fb {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    int prime_fd;
    void *map;
};
static struct shadow_fb g_shadow[SHADOW_COUNT];
static int g_shadow_ready;
static int g_shadow_next;
static int g_dumped;
static long g_frame_count;

typedef int (*c_RkRgaInit_t)(void);
typedef int (*c_RkRgaBlit_t)(leaf_rga_info_t *, leaf_rga_info_t *, leaf_rga_info_t *);
static c_RkRgaBlit_t g_rga_blit;
static int g_rga_ready = -1;

union leaf_ioctl_sym {
    void *object;
    int (*func)(int, unsigned long, ...);
};

union leaf_rga_init_sym {
    void *object;
    c_RkRgaInit_t func;
};

union leaf_rga_blit_sym {
    void *object;
    c_RkRgaBlit_t func;
};

static void leaf_log(const char *fmt, ...)
{
    if (!g_debug) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[leaf-drm-rotate] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void shim_init(void)
{
    if (g_initialized) {
        return;
    }
    g_initialized = 1;
    union leaf_ioctl_sym ioctl_sym = { .object = dlsym(RTLD_NEXT, "ioctl") };
    real_ioctl = ioctl_sym.func;
    const char *rot = getenv("LEAF_DRM_ROTATE");
    g_rotate = rot ? atoi(rot) : 0;
    if (g_rotate != 90 && g_rotate != 270) {
        g_rotate = 0;
    }
    const char *dbg = getenv("LEAF_DRM_ROTATE_DEBUG");
    g_debug = dbg && dbg[0] && dbg[0] != '0';
    leaf_log("loaded rotate=%d", g_rotate);
}

static int rga_ready(void)
{
    if (g_rga_ready >= 0) {
        return g_rga_ready;
    }
    g_rga_ready = 0;
    void *lib = dlopen("librga.so.2", RTLD_NOW);
    if (!lib) {
        leaf_log("dlopen librga.so.2 failed: %s", dlerror());
        return 0;
    }
    union leaf_rga_init_sym init_sym = { .object = dlsym(lib, "c_RkRgaInit") };
    union leaf_rga_blit_sym blit_sym = { .object = dlsym(lib, "c_RkRgaBlit") };
    c_RkRgaInit_t init = init_sym.func;
    g_rga_blit = blit_sym.func;
    if (!init || !g_rga_blit || init() != 0) {
        leaf_log("librga init failed");
        g_rga_blit = NULL;
        return 0;
    }
    g_rga_ready = 1;
    leaf_log("librga ready");
    return 1;
}

static struct tracked_fb *fb_find(uint32_t fb_id)
{
    for (int i = 0; i < FB_TABLE_MAX; i++) {
        if (g_fbs[i].fb_id == fb_id) {
            return &g_fbs[i];
        }
    }
    return NULL;
}

static void fb_track(int drm_fd,
                     uint32_t fb_id,
                     uint32_t width,
                     uint32_t height,
                     uint32_t pitch,
                     uint32_t handle)
{
    struct tracked_fb *slot = fb_find(fb_id);
    if (!slot) {
        slot = fb_find(0);
    }
    if (!slot) {
        return;
    }
    if (slot->prime_fd > 0) {
        close(slot->prime_fd);
    }
    slot->fb_id = fb_id;
    slot->width = width;
    slot->height = height;
    slot->pitch = pitch;
    slot->handle = handle;
    slot->prime_fd = -1;
    struct drm_prime_handle req = { .handle = handle, .flags = O_CLOEXEC };
    if (real_ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &req) == 0) {
        slot->prime_fd = req.fd;
    }
    leaf_log("track fb %u %ux%u pitch=%u handle=%u prime_fd=%d",
             fb_id,
             width,
             height,
             pitch,
             handle,
             slot->prime_fd);
}

static void fb_untrack(uint32_t fb_id)
{
    struct tracked_fb *slot = fb_find(fb_id);
    if (!slot) {
        return;
    }
    if (slot->prime_fd > 0) {
        close(slot->prime_fd);
    }
    memset(slot, 0, sizeof(*slot));
    slot->prime_fd = -1;
}

static int shadows_create(int drm_fd)
{
    if (g_shadow_ready) {
        return 1;
    }
    if (!g_have_real_mode) {
        leaf_log("no real mode cached; cannot build shadows");
        return 0;
    }
    for (int i = 0; i < SHADOW_COUNT; i++) {
        struct drm_mode_create_dumb creq = {
            .width = g_real_mode.hdisplay,
            .height = g_real_mode.vdisplay,
            .bpp = 32,
        };
        if (real_ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
            leaf_log("shadow CREATE_DUMB failed: %s", strerror(errno));
            return 0;
        }
        struct drm_mode_fb_cmd2 fbreq = {
            .width = creq.width,
            .height = creq.height,
            .pixel_format = DRM_FORMAT_XRGB8888,
        };
        fbreq.handles[0] = creq.handle;
        fbreq.pitches[0] = creq.pitch;
        if (real_ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fbreq) < 0) {
            leaf_log("shadow ADDFB2 failed: %s", strerror(errno));
            return 0;
        }
        struct drm_prime_handle preq = { .handle = creq.handle, .flags = O_CLOEXEC };
        if (real_ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &preq) < 0) {
            leaf_log("shadow prime export failed: %s", strerror(errno));
            return 0;
        }
        g_shadow[i].handle = creq.handle;
        g_shadow[i].pitch = creq.pitch;
        g_shadow[i].size = creq.size;
        g_shadow[i].fb_id = fbreq.fb_id;
        g_shadow[i].prime_fd = preq.fd;
        g_shadow[i].map = NULL;
    }
    g_shadow_ready = 1;
    leaf_log("shadows ready: %ux%u x%d",
             g_real_mode.hdisplay,
             g_real_mode.vdisplay,
             SHADOW_COUNT);
    return 1;
}

static void shadow_dump(int drm_fd, struct shadow_fb *shadow)
{
    const char *path = getenv("LEAF_DRM_ROTATE_DUMP");
    if (!path || g_dumped) {
        return;
    }
    const char *at = getenv("LEAF_DRM_ROTATE_DUMP_FRAME");
    if (at && g_frame_count < atol(at)) {
        return;
    }
    g_dumped = 1;
    if (!shadow->map) {
        struct drm_mode_map_dumb mreq = { .handle = shadow->handle };
        if (real_ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
            return;
        }
        shadow->map = mmap(0, shadow->size, PROT_READ, MAP_SHARED, drm_fd, mreq.offset);
        if (shadow->map == MAP_FAILED) {
            shadow->map = NULL;
            return;
        }
    }
    FILE *fp = fopen(path, "we");
    if (!fp) {
        return;
    }
    fwrite(shadow->map, 1, shadow->size, fp);
    fclose(fp);
    leaf_log("dumped rotated frame to %s (%ux%u pitch=%u)",
             path,
             g_real_mode.hdisplay,
             g_real_mode.vdisplay,
             shadow->pitch);
}

static uint32_t rotate_into_shadow(int drm_fd, uint32_t fb_id)
{
    struct tracked_fb *fb = fb_find(fb_id);
    if (!fb || fb->prime_fd < 0 || !rga_ready() || !shadows_create(drm_fd)) {
        return 0;
    }

    struct shadow_fb *shadow = &g_shadow[g_shadow_next];
    g_shadow_next = (g_shadow_next + 1) % SHADOW_COUNT;

    leaf_rga_info_t src;
    leaf_rga_info_t dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.fd = fb->prime_fd;
    src.mmuFlag = 1;
    src.rotation = (g_rotate == 90) ? HAL_TRANSFORM_ROT_90 : HAL_TRANSFORM_ROT_270;
    leaf_rga_set_rect(&src.rect,
                      0,
                      0,
                      (int)fb->width,
                      (int)fb->height,
                      (int)(fb->pitch / 4),
                      (int)fb->height,
                      RK_FORMAT_BGRA_8888);
    dst.fd = shadow->prime_fd;
    dst.mmuFlag = 1;
    leaf_rga_set_rect(&dst.rect,
                      0,
                      0,
                      g_real_mode.hdisplay,
                      g_real_mode.vdisplay,
                      (int)(shadow->pitch / 4),
                      g_real_mode.vdisplay,
                      RK_FORMAT_BGRA_8888);
    int rc = g_rga_blit(&src, &dst, NULL);
    if (rc != 0) {
        leaf_log("rga blit failed rc=%d (fb %u %ux%u)", rc, fb_id, fb->width, fb->height);
        return 0;
    }
    g_frame_count++;
    shadow_dump(drm_fd, shadow);
    return shadow->fb_id;
}

static void mode_swap_wh(struct drm_mode_modeinfo *mode)
{
    uint16_t temp;
    temp = mode->hdisplay;
    mode->hdisplay = mode->vdisplay;
    mode->vdisplay = temp;
    temp = mode->hsync_start;
    mode->hsync_start = mode->vsync_start;
    mode->vsync_start = temp;
    temp = mode->hsync_end;
    mode->hsync_end = mode->vsync_end;
    mode->vsync_end = temp;
    temp = mode->htotal;
    mode->htotal = mode->vtotal;
    mode->vtotal = temp;
}

static int handle_getconnector(int fd, struct drm_mode_get_connector *connector)
{
    int rc = real_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, connector);
    if (rc != 0 || !g_rotate) {
        return rc;
    }
    struct drm_mode_modeinfo *modes =
        (struct drm_mode_modeinfo *)(uintptr_t)connector->modes_ptr;
    if (!modes || !connector->count_modes) {
        return rc;
    }
    if (!g_have_real_mode) {
        g_real_mode = modes[0];
        g_have_real_mode = 1;
        leaf_log("real mode cached: %ux%u@%u",
                 g_real_mode.hdisplay,
                 g_real_mode.vdisplay,
                 g_real_mode.vrefresh);
    }
    for (uint32_t i = 0; i < connector->count_modes; i++) {
        mode_swap_wh(&modes[i]);
    }
    uint32_t temp = connector->mm_width;
    connector->mm_width = connector->mm_height;
    connector->mm_height = temp;
    leaf_log("GETCONNECTOR %u: reported %u mode(s) swapped to landscape",
             connector->connector_id,
             connector->count_modes);
    return rc;
}

static int handle_getcrtc(int fd, struct drm_mode_crtc *crtc)
{
    int rc = real_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, crtc);
    if (rc == 0 && g_rotate && crtc->mode_valid) {
        mode_swap_wh(&crtc->mode);
    }
    return rc;
}

static int handle_setcrtc(int fd, struct drm_mode_crtc *crtc)
{
    if (!g_rotate) {
        return real_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, crtc);
    }

    struct drm_mode_crtc fixed = *crtc;
    if (g_have_real_mode && fixed.mode_valid) {
        fixed.mode = g_real_mode;
    }
    uint32_t shadow_id = fixed.fb_id ? rotate_into_shadow(fd, fixed.fb_id) : 0;
    if (shadow_id) {
        fixed.fb_id = shadow_id;
    }
    leaf_log("SETCRTC crtc=%u fb=%u -> fb=%u mode=%ux%u",
             crtc->crtc_id,
             crtc->fb_id,
             fixed.fb_id,
             fixed.mode.hdisplay,
             fixed.mode.vdisplay);
    int rc = real_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &fixed);
    if (rc != 0) {
        leaf_log("SETCRTC failed: %s", strerror(errno));
    }
    return rc;
}

static int handle_pageflip(int fd, struct drm_mode_crtc_page_flip *flip)
{
    if (!g_rotate) {
        return real_ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, flip);
    }
    uint32_t shadow_id = rotate_into_shadow(fd, flip->fb_id);
    if (!shadow_id) {
        return real_ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, flip);
    }
    struct drm_mode_crtc_page_flip fixed = *flip;
    fixed.fb_id = shadow_id;
    return real_ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &fixed);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    shim_init();
    if (!real_ioctl) {
        errno = ENOSYS;
        return -1;
    }

    if (_IOC_TYPE(request) != DRM_IOCTL_BASE || !g_rotate) {
        return real_ioctl(fd, request, arg);
    }

    pthread_mutex_lock(&g_lock);
    int rc;
    switch (request) {
    case DRM_IOCTL_MODE_GETCONNECTOR:
        rc = handle_getconnector(fd, arg);
        break;
    case DRM_IOCTL_MODE_GETCRTC:
        rc = handle_getcrtc(fd, arg);
        break;
    case DRM_IOCTL_MODE_SETCRTC:
        rc = handle_setcrtc(fd, arg);
        break;
    case DRM_IOCTL_MODE_PAGE_FLIP:
        rc = handle_pageflip(fd, arg);
        break;
    case DRM_IOCTL_MODE_ADDFB2: {
        struct drm_mode_fb_cmd2 *req = arg;
        rc = real_ioctl(fd, request, arg);
        if (rc == 0) {
            fb_track(fd, req->fb_id, req->width, req->height, req->pitches[0], req->handles[0]);
        }
        break;
    }
    case DRM_IOCTL_MODE_ADDFB: {
        struct drm_mode_fb_cmd *req = arg;
        rc = real_ioctl(fd, request, arg);
        if (rc == 0) {
            fb_track(fd, req->fb_id, req->width, req->height, req->pitch, req->handle);
        }
        break;
    }
    case DRM_IOCTL_MODE_RMFB: {
        uint32_t *fb_id = arg;
        fb_untrack(*fb_id);
        rc = real_ioctl(fd, request, arg);
        break;
    }
    default:
        rc = real_ioctl(fd, request, arg);
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}
