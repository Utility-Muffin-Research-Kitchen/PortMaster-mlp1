#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *leaf_mali;

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native_display, const EGLint *attrib_list);
EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attrib_list);
EGLSurface eglCreatePlatformPixmapSurfaceEXT(EGLDisplay dpy, EGLConfig config, void *native_pixmap, const EGLint *attrib_list);

static void *leaf_sym(const char *name)
{
    if (!leaf_mali) {
        const char *override = getenv("LEAF_PM_MALI_LIB");
        if (override && override[0]) {
            leaf_mali = dlopen(override, RTLD_LAZY | RTLD_GLOBAL);
        }
        if (!leaf_mali) {
            leaf_mali = dlopen("libmali.so.1", RTLD_LAZY | RTLD_GLOBAL);
        }
        if (!leaf_mali) {
            leaf_mali = dlopen("/lib/libmali.so.1", RTLD_LAZY | RTLD_GLOBAL);
        }
        if (!leaf_mali) {
            abort();
        }
    }

    void *p = dlsym(leaf_mali, name);
    if (!p && strcmp(name, "eglGetProcAddress") != 0) {
        typedef __eglMustCastToProperFunctionPointerType (*get_proc_t)(const char *);
        get_proc_t get_proc = (get_proc_t)dlsym(leaf_mali, "eglGetProcAddress");
        if (get_proc) {
            p = (void *)get_proc(name);
        }
    }
    return p;
}

static void *leaf_sym_any(const char *name, const char *fallback)
{
    void *p = leaf_sym(name);
    return p ? p : leaf_sym(fallback);
}

static int leaf_debug_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("LEAF_EGL_DEBUG");
        v = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return v;
}

static unsigned long leaf_frame_counter;

static void leaf_rewrite_context_attrs(const EGLint *in, EGLint *out, size_t out_count)
{
    if (!in || !out || out_count < 2) {
        return;
    }

    size_t j = 0;
    for (size_t i = 0; j + 1 < out_count; i += 2) {
        out[j++] = in[i];
        if (in[i] == EGL_NONE) {
            return;
        }

        EGLint value = in[i + 1];
        if (in[i] == EGL_CONTEXT_MINOR_VERSION_KHR && value > 2) {
            value = 2;
        }
        out[j++] = value;
    }
    out[out_count - 1] = EGL_NONE;
}

static void leaf_rewrite_attribs_to_int(const EGLAttrib *in, EGLint *out, size_t out_count)
{
    if (!in || !out || out_count < 2) {
        return;
    }

    size_t j = 0;
    for (size_t i = 0; j + 1 < out_count; i += 2) {
        out[j++] = (EGLint)in[i];
        if (in[i] == EGL_NONE) {
            return;
        }
        out[j++] = (EGLint)in[i + 1];
    }
    out[out_count - 1] = EGL_NONE;
}

#define LEAF_EGL_FWD(ret, name, args, callargs) \
    ret name args \
    { \
        typedef ret (*fn_t) args; \
        static fn_t fn; \
        if (!fn) { \
            fn = (fn_t)leaf_sym(#name); \
        } \
        return fn callargs; \
    }

LEAF_EGL_FWD(EGLint, eglGetError, (void), ())
LEAF_EGL_FWD(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType display_id), (display_id))

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    typedef EGLBoolean (*fn_t)(EGLDisplay, EGLint *, EGLint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("eglInitialize");
    }
    return fn(dpy, major, minor);
}

LEAF_EGL_FWD(EGLBoolean, eglTerminate, (EGLDisplay dpy), (dpy))

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    typedef const char *(*fn_t)(EGLDisplay, EGLint);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("eglQueryString");
    }

    const char *value = fn(dpy, name);
    static char version[128];
    static char extensions[4096];

    if (value && dpy != EGL_NO_DISPLAY && name == EGL_VERSION) {
        const char *rest = strchr(value, ' ');
        while (rest && *rest == ' ') {
            rest++;
        }
        snprintf(version, sizeof(version), "1.5%s%s", rest && *rest ? " " : "", rest && *rest ? rest : "");
        return version;
    }

    if (value && dpy != EGL_NO_DISPLAY && name == EGL_EXTENSIONS) {
        snprintf(extensions, sizeof(extensions), "%s %s", value,
                 "EGL_EXT_platform_base EGL_KHR_platform_wayland EGL_EXT_platform_wayland");
        return extensions;
    }

    return value;
}

LEAF_EGL_FWD(EGLBoolean, eglGetConfigs, (EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config), (dpy, configs, config_size, num_config))
LEAF_EGL_FWD(EGLBoolean, eglChooseConfig, (EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config), (dpy, attrib_list, configs, config_size, num_config))
LEAF_EGL_FWD(EGLBoolean, eglGetConfigAttrib, (EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value), (dpy, config, attribute, value))

static void leaf_log_config(EGLDisplay dpy, EGLConfig config, const char *where)
{
    typedef EGLBoolean (*fn_t)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    fn_t get_attrib = (fn_t)leaf_sym("eglGetConfigAttrib");
    EGLint id = -1, r = -1, g = -1, b = -1, a = -1, depth = -1, stencil = -1, sample_buffers = -1, samples = -1;
    get_attrib(dpy, config, EGL_CONFIG_ID, &id);
    get_attrib(dpy, config, EGL_RED_SIZE, &r);
    get_attrib(dpy, config, EGL_GREEN_SIZE, &g);
    get_attrib(dpy, config, EGL_BLUE_SIZE, &b);
    get_attrib(dpy, config, EGL_ALPHA_SIZE, &a);
    get_attrib(dpy, config, EGL_DEPTH_SIZE, &depth);
    get_attrib(dpy, config, EGL_STENCIL_SIZE, &stencil);
    get_attrib(dpy, config, EGL_SAMPLE_BUFFERS, &sample_buffers);
    get_attrib(dpy, config, EGL_SAMPLES, &samples);
    fprintf(stderr, "[leaf-egl] %s: config id=%d rgba=%d%d%d%d depth=%d stencil=%d sample_buffers=%d samples=%d\n",
            where, id, r, g, b, a, depth, stencil, sample_buffers, samples);
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list)
{
    typedef EGLSurface (*fn_t)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("eglCreateWindowSurface");
    }
    if (leaf_debug_enabled()) {
        leaf_log_config(dpy, config, "eglCreateWindowSurface");
    }
    return fn(dpy, config, win, attrib_list);
}
LEAF_EGL_FWD(EGLSurface, eglCreatePbufferSurface, (EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list), (dpy, config, attrib_list))
LEAF_EGL_FWD(EGLSurface, eglCreatePixmapSurface, (EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap, const EGLint *attrib_list), (dpy, config, pixmap, attrib_list))
LEAF_EGL_FWD(EGLBoolean, eglDestroySurface, (EGLDisplay dpy, EGLSurface surface), (dpy, surface))
LEAF_EGL_FWD(EGLBoolean, eglQuerySurface, (EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value), (dpy, surface, attribute, value))

EGLBoolean eglBindAPI(EGLenum api)
{
    typedef EGLBoolean (*fn_t)(EGLenum);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("eglBindAPI");
    }
    if (api == EGL_OPENGL_API) {
        api = EGL_OPENGL_ES_API;
    }
    return fn(api);
}

LEAF_EGL_FWD(EGLenum, eglQueryAPI, (void), ())
LEAF_EGL_FWD(EGLBoolean, eglWaitClient, (void), ())
LEAF_EGL_FWD(EGLBoolean, eglReleaseThread, (void), ())
LEAF_EGL_FWD(EGLSurface, eglCreatePbufferFromClientBuffer, (EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config, const EGLint *attrib_list), (dpy, buftype, buffer, config, attrib_list))
LEAF_EGL_FWD(EGLBoolean, eglSurfaceAttrib, (EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value), (dpy, surface, attribute, value))
LEAF_EGL_FWD(EGLBoolean, eglBindTexImage, (EGLDisplay dpy, EGLSurface surface, EGLint buffer), (dpy, surface, buffer))
LEAF_EGL_FWD(EGLBoolean, eglReleaseTexImage, (EGLDisplay dpy, EGLSurface surface, EGLint buffer), (dpy, surface, buffer))
LEAF_EGL_FWD(EGLBoolean, eglSwapInterval, (EGLDisplay dpy, EGLint interval), (dpy, interval))

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list)
{
    typedef EGLContext (*fn_t)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("eglCreateContext");
    }

    EGLint rewritten[48];
    if (attrib_list) {
        leaf_rewrite_context_attrs(attrib_list, rewritten, sizeof(rewritten) / sizeof(rewritten[0]));
        attrib_list = rewritten;
    }
    return fn(dpy, config, share_context, attrib_list);
}

LEAF_EGL_FWD(EGLBoolean, eglDestroyContext, (EGLDisplay dpy, EGLContext ctx), (dpy, ctx))
LEAF_EGL_FWD(EGLBoolean, eglMakeCurrent, (EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx), (dpy, draw, read, ctx))
LEAF_EGL_FWD(EGLContext, eglGetCurrentContext, (void), ())
LEAF_EGL_FWD(EGLSurface, eglGetCurrentSurface, (EGLint readdraw), (readdraw))
LEAF_EGL_FWD(EGLDisplay, eglGetCurrentDisplay, (void), ())
LEAF_EGL_FWD(EGLBoolean, eglQueryContext, (EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value), (dpy, ctx, attribute, value))
LEAF_EGL_FWD(EGLBoolean, eglWaitGL, (void), ())
LEAF_EGL_FWD(EGLBoolean, eglWaitNative, (EGLint engine), (engine))

static void leaf_debug_swap(EGLDisplay dpy, EGLSurface surface)
{
    typedef unsigned int GLenum;
    typedef unsigned int GLuint;
    typedef int GLint;
    typedef int GLsizei;
    static void (*p_glGetIntegerv)(GLenum, GLint *);
    static GLenum (*p_glGetError)(void);
    static void (*p_glBindFramebuffer)(GLenum, GLuint);
    static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    if (!p_glGetIntegerv) {
        p_glGetIntegerv = (void (*)(GLenum, GLint *))leaf_sym("glGetIntegerv");
        p_glGetError = (GLenum (*)(void))leaf_sym("glGetError");
        p_glBindFramebuffer = (void (*)(GLenum, GLuint))leaf_sym("glBindFramebuffer");
        p_glReadPixels = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))leaf_sym("glReadPixels");
    }
    if (!p_glGetIntegerv || !p_glGetError || !p_glBindFramebuffer || !p_glReadPixels) {
        return;
    }

    static unsigned long swap_count;
    swap_count++;
    if (swap_count > 10 && swap_count % 60 != 0) {
        return;
    }

    GLenum pre_err = p_glGetError();
    GLint prev_read = 0;
    p_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &prev_read);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
    p_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    unsigned char center[4] = {0}, corner[4] = {0};
    p_glReadPixels(w / 2, h / 2, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, center);
    p_glReadPixels(8, 8, 1, 1, 0x1908, 0x1401, corner);
    GLenum post_err = p_glGetError();
    p_glBindFramebuffer(0x8CA8, (GLuint)prev_read);
    fprintf(stderr,
            "[leaf-egl] swap #%lu %dx%d prev_read_fbo=%d center=%02x%02x%02x%02x corner=%02x%02x%02x%02x pre_err=0x%x post_err=0x%x\n",
            swap_count, w, h, prev_read,
            center[0], center[1], center[2], center[3],
            corner[0], corner[1], corner[2], corner[3],
            pre_err, post_err);
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    typedef EGLBoolean (*fn_t)(EGLDisplay, EGLSurface);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("eglSwapBuffers");
    }
    leaf_frame_counter++;
    if (leaf_debug_enabled()) {
        leaf_debug_swap(dpy, surface);
    }
    return fn(dpy, surface);
}
LEAF_EGL_FWD(EGLBoolean, eglCopyBuffers, (EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target), (dpy, surface, target))

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native_display, const EGLint *attrib_list)
{
    (void)platform;
    (void)attrib_list;
    return eglGetDisplay((EGLNativeDisplayType)native_display);
}

EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attrib_list)
{
    return eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)native_window, attrib_list);
}

EGLSurface eglCreatePlatformPixmapSurfaceEXT(EGLDisplay dpy, EGLConfig config, void *native_pixmap, const EGLint *attrib_list)
{
    return eglCreatePixmapSurface(dpy, config, (EGLNativePixmapType)native_pixmap, attrib_list);
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display, const EGLAttrib *attrib_list)
{
    (void)platform;
    (void)attrib_list;
    return eglGetDisplay((EGLNativeDisplayType)native_display);
}

EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLAttrib *attrib_list)
{
    (void)attrib_list;
    return eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)native_window, NULL);
}

EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void *native_pixmap, const EGLAttrib *attrib_list)
{
    (void)attrib_list;
    return eglCreatePixmapSurface(dpy, config, (EGLNativePixmapType)native_pixmap, NULL);
}

EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list)
{
    typedef EGLSync (*fn_t)(EGLDisplay, EGLenum, const EGLint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym_any("eglCreateSync", "eglCreateSyncKHR");
    }
    EGLint rewritten[48];
    if (attrib_list) {
        leaf_rewrite_attribs_to_int(attrib_list, rewritten, sizeof(rewritten) / sizeof(rewritten[0]));
        return fn(dpy, type, rewritten);
    }
    return fn(dpy, type, NULL);
}

EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync)
{
    typedef EGLBoolean (*fn_t)(EGLDisplay, EGLSync);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym_any("eglDestroySync", "eglDestroySyncKHR");
    }
    return fn(dpy, sync);
}

EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout)
{
    typedef EGLint (*fn_t)(EGLDisplay, EGLSync, EGLint, EGLTime);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym_any("eglClientWaitSync", "eglClientWaitSyncKHR");
    }
    return fn(dpy, sync, flags, timeout);
}

EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib *value)
{
    typedef EGLBoolean (*fn_t)(EGLDisplay, EGLSync, EGLint, EGLint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym_any("eglGetSyncAttrib", "eglGetSyncAttribKHR");
    }
    EGLint int_value = 0;
    EGLBoolean ok = fn(dpy, sync, attribute, &int_value);
    if (value) {
        *value = (EGLAttrib)int_value;
    }
    return ok;
}

EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags)
{
    typedef EGLBoolean (*fn_t)(EGLDisplay, EGLSync, EGLint);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym_any("eglWaitSync", "eglWaitSyncKHR");
    }
    return fn(dpy, sync, flags);
}

EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attrib_list)
{
    typedef EGLImage (*fn_t)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym_any("eglCreateImage", "eglCreateImageKHR");
    }
    EGLint rewritten[48];
    if (attrib_list) {
        leaf_rewrite_attribs_to_int(attrib_list, rewritten, sizeof(rewritten) / sizeof(rewritten[0]));
        return fn(dpy, ctx, target, buffer, rewritten);
    }
    return fn(dpy, ctx, target, buffer, NULL);
}

EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage image)
{
    typedef EGLBoolean (*fn_t)(EGLDisplay, EGLImage);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym_any("eglDestroyImage", "eglDestroyImageKHR");
    }
    return fn(dpy, image);
}

/* --- LEAF_EGL_DEBUG=1 GL call tracing ------------------------------------
 * Godot's glad loader resolves all GLES entry points via eglGetProcAddress
 * on this stack (it never dlopens libGLESv2), so debug wrappers for the
 * framebuffer-related calls are dispatched from eglGetProcAddress below. */

typedef unsigned int LeafGLenum;
typedef unsigned int LeafGLuint;
typedef unsigned int LeafGLbitfield;
typedef int LeafGLint;
typedef int LeafGLsizei;

static LeafGLenum leaf_gl_error(void)
{
    static LeafGLenum (*fn)(void);
    if (!fn) {
        fn = (LeafGLenum (*)(void))leaf_sym("glGetError");
    }
    return fn ? fn() : 0;
}

#define LEAF_GL_LOG(...) fprintf(stderr, "[leaf-gl] " __VA_ARGS__)

static void leaf_glBlitFramebuffer(LeafGLint sx0, LeafGLint sy0, LeafGLint sx1, LeafGLint sy1,
                                   LeafGLint dx0, LeafGLint dy0, LeafGLint dx1, LeafGLint dy1,
                                   LeafGLbitfield mask, LeafGLenum filter)
{
    typedef void (*fn_t)(LeafGLint, LeafGLint, LeafGLint, LeafGLint, LeafGLint, LeafGLint, LeafGLint, LeafGLint, LeafGLbitfield, LeafGLenum);
    typedef LeafGLenum (*status_t)(LeafGLenum);
    static fn_t fn;
    static status_t status;
    if (!fn) {
        fn = (fn_t)leaf_sym("glBlitFramebuffer");
        status = (status_t)leaf_sym("glCheckFramebufferStatus");
    }

    LeafGLenum pre = leaf_gl_error();
    fn(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    LeafGLenum post = leaf_gl_error();
    static unsigned long n;
    n++;
    if (leaf_debug_enabled() && (n <= 20 || post != 0 || n % 120 == 0)) {
        LeafGLenum rs = status(0x8CA8 /* GL_READ_FRAMEBUFFER */);
        LeafGLenum ds = status(0x8CA9 /* GL_DRAW_FRAMEBUFFER */);
        LEAF_GL_LOG("blit #%lu src=(%d,%d,%d,%d) dst=(%d,%d,%d,%d) mask=0x%x filter=0x%x pre=0x%x post=0x%x read_status=0x%x draw_status=0x%x\n",
                    n, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter, pre, post, rs, ds);
    }
}

static void leaf_glInvalidateFramebuffer(LeafGLenum target, LeafGLsizei num, const LeafGLenum *att)
{
    typedef void (*fn_t)(LeafGLenum, LeafGLsizei, const LeafGLenum *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glInvalidateFramebuffer");
    }
    static unsigned long n;
    n++;
    if (n <= 40 || n % 240 == 0) {
        char buf[128] = "";
        for (LeafGLsizei i = 0; i < num && i < 4; i++) {
            char one[24];
            snprintf(one, sizeof(one), " 0x%x", att[i]);
            strncat(buf, one, sizeof(buf) - strlen(buf) - 1);
        }
        LEAF_GL_LOG("invalidate #%lu target=0x%x num=%d att=%s\n", n, target, num, buf);
    }
    fn(target, num, att);
}

static void leaf_glBindFramebuffer(LeafGLenum target, LeafGLuint fbo)
{
    typedef void (*fn_t)(LeafGLenum, LeafGLuint);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glBindFramebuffer");
    }
    static unsigned long n;
    n++;
    if (n <= 80) {
        LEAF_GL_LOG("bindfb #%lu target=0x%x fbo=%u\n", n, target, fbo);
    }
    fn(target, fbo);
}

static void leaf_glTexStorage2D(LeafGLenum target, LeafGLsizei levels, LeafGLenum ifmt, LeafGLsizei w, LeafGLsizei h)
{
    typedef void (*fn_t)(LeafGLenum, LeafGLsizei, LeafGLenum, LeafGLsizei, LeafGLsizei);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glTexStorage2D");
    }
    LeafGLenum pre = leaf_gl_error();
    fn(target, levels, ifmt, w, h);
    LeafGLenum post = leaf_gl_error();
    static unsigned long n;
    n++;
    if (n <= 60 || post != 0) {
        LEAF_GL_LOG("texstorage2d #%lu target=0x%x levels=%d ifmt=0x%x %dx%d pre=0x%x post=0x%x\n",
                    n, target, levels, ifmt, w, h, pre, post);
    }
}

static void leaf_glScissor(LeafGLint x, LeafGLint y, LeafGLsizei w, LeafGLsizei h)
{
    typedef void (*fn_t)(LeafGLint, LeafGLint, LeafGLsizei, LeafGLsizei);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glScissor");
    }
    static unsigned long n;
    n++;
    if (n <= 40 || w <= 0 || h <= 0) {
        LEAF_GL_LOG("scissor #%lu %d,%d %dx%d\n", n, x, y, w, h);
    }
    fn(x, y, w, h);
}

static void leaf_glClear(LeafGLbitfield mask)
{
    typedef void (*fn_t)(LeafGLbitfield);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glClear");
    }
    LeafGLenum pre = leaf_gl_error();
    fn(mask);
    LeafGLenum post = leaf_gl_error();
    static unsigned long n;
    n++;
    if (n <= 40 || post != 0) {
        LEAF_GL_LOG("clear #%lu mask=0x%x pre=0x%x post=0x%x\n", n, mask, pre, post);
    }
}

static LeafGLenum leaf_glCheckFramebufferStatus(LeafGLenum target)
{
    typedef LeafGLenum (*fn_t)(LeafGLenum);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glCheckFramebufferStatus");
    }
    LeafGLenum s = fn(target);
    static unsigned long n;
    n++;
    if (n <= 40 || s != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) {
        LEAF_GL_LOG("fbstatus #%lu target=0x%x -> 0x%x\n", n, target, s);
    }
    return s;
}

/* 0 = off, 1 = glFinish after each draw, 2 = glFlush after each draw,
 * 3 = strengthen glClientWaitSync to a real glFinish,
 * 4 = glFinish before buffer/texture uploads that follow pending draws */
static int leaf_draw_finish_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("LEAF_EGL_DRAW_FINISH");
        if (!e || !e[0] || strcmp(e, "0") == 0) {
            v = 0;
        } else if (strcmp(e, "flush") == 0 || strcmp(e, "2") == 0) {
            v = 2;
        } else if (strcmp(e, "sync") == 0 || strcmp(e, "3") == 0) {
            v = 3;
        } else if (strcmp(e, "upload") == 0 || strcmp(e, "4") == 0) {
            v = 4;
        } else {
            v = 1;
        }
    }
    return v;
}

static int leaf_draw_finish_enabled(void)
{
    int m = leaf_draw_finish_mode();
    return m == 1 || m == 2 || m == 4;
}

static int leaf_pending_draws;

static void leaf_gl_finish_before_upload(void)
{
    typedef void (*finish_t)(void);
    static finish_t p_finish;
    if (!leaf_pending_draws) {
        return;
    }
    if (!p_finish) {
        p_finish = (finish_t)leaf_sym("glFinish");
    }
    p_finish();
    leaf_pending_draws = 0;
}

static void leaf_glBufferData(LeafGLenum target, long size, const void *data, LeafGLenum usage)
{
    typedef void (*fn_t)(LeafGLenum, long, const void *, LeafGLenum);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glBufferData");
    }
    if (leaf_draw_finish_mode() == 4) {
        leaf_gl_finish_before_upload();
    }
    fn(target, size, data, usage);
}

static void leaf_glBufferSubData(LeafGLenum target, long offset, long size, const void *data)
{
    typedef void (*fn_t)(LeafGLenum, long, long, const void *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glBufferSubData");
    }
    if (leaf_draw_finish_mode() == 4) {
        leaf_gl_finish_before_upload();
    }
    fn(target, offset, size, data);
}

static void leaf_glTexSubImage2D(LeafGLenum target, LeafGLint level, LeafGLint x, LeafGLint y,
                                 LeafGLsizei w, LeafGLsizei h, LeafGLenum format, LeafGLenum type, const void *pixels)
{
    typedef void (*fn_t)(LeafGLenum, LeafGLint, LeafGLint, LeafGLint, LeafGLsizei, LeafGLsizei, LeafGLenum, LeafGLenum, const void *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glTexSubImage2D");
    }
    if (leaf_draw_finish_mode() == 4) {
        leaf_gl_finish_before_upload();
    }
    fn(target, level, x, y, w, h, format, type, pixels);
}

static void leaf_glDeleteBuffers(LeafGLsizei n, const LeafGLuint *ids)
{
    typedef void (*fn_t)(LeafGLsizei, const LeafGLuint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glDeleteBuffers");
    }
    if (leaf_draw_finish_mode() == 4) {
        leaf_gl_finish_before_upload();
    }
    fn(n, ids);
}

static void leaf_glDeleteTextures(LeafGLsizei n, const LeafGLuint *ids)
{
    typedef void (*fn_t)(LeafGLsizei, const LeafGLuint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glDeleteTextures");
    }
    if (leaf_draw_finish_mode() == 4) {
        leaf_gl_finish_before_upload();
    }
    fn(n, ids);
}

static void leaf_glDeleteVertexArrays(LeafGLsizei n, const LeafGLuint *ids)
{
    typedef void (*fn_t)(LeafGLsizei, const LeafGLuint *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glDeleteVertexArrays");
    }
    if (leaf_draw_finish_mode() == 4) {
        leaf_gl_finish_before_upload();
    }
    fn(n, ids);
}

static void *leaf_glMapBufferRange(LeafGLenum target, long offset, long length, LeafGLbitfield access)
{
    typedef void *(*fn_t)(LeafGLenum, long, long, LeafGLbitfield);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glMapBufferRange");
    }
    if (leaf_draw_finish_mode() == 4) {
        leaf_gl_finish_before_upload();
        access &= ~(LeafGLbitfield)0x0020; /* GL_MAP_UNSYNCHRONIZED_BIT */
    }
    return fn(target, offset, length, access);
}

/* Mode 3: the Mali blob's GL sync objects appear to signal before the GPU is
 * done reading streamed vertex data, letting Godot overwrite in-flight
 * buffers (kernel: "Unhandled Page fault", jobs TERMINATED, black screen).
 * Turning client waits into real finishes restores correctness at a
 * once-per-frame cost. */
static LeafGLenum leaf_glClientWaitSync(void *sync, LeafGLbitfield flags, unsigned long long timeout)
{
    typedef LeafGLenum (*fn_t)(void *, LeafGLbitfield, unsigned long long);
    typedef void (*finish_t)(void);
    static fn_t fn;
    static finish_t p_finish;
    if (!fn) {
        fn = (fn_t)leaf_sym("glClientWaitSync");
        p_finish = (finish_t)leaf_sym("glFinish");
    }
    if (leaf_draw_finish_mode() == 3) {
        p_finish();
    }
    return fn(sync, flags, timeout);
}

static void leaf_gl_draw_check(const char *what, LeafGLenum mode, LeafGLint count, LeafGLint instances)
{
    typedef void (*finish_t)(void);
    typedef void (*getintv_t)(LeafGLenum, LeafGLint *);
    typedef void (*activetexture_t)(LeafGLenum);
    static finish_t p_finish, p_flush;
    static getintv_t p_getintv;
    static activetexture_t p_activetexture;
    if (!p_finish) {
        p_finish = (finish_t)leaf_sym("glFinish");
        p_flush = (finish_t)leaf_sym("glFlush");
        p_getintv = (getintv_t)leaf_sym("glGetIntegerv");
        p_activetexture = (activetexture_t)leaf_sym("glActiveTexture");
    }
    static unsigned long n;
    static int reported;
    n++;
    if (leaf_draw_finish_mode() == 4) {
        leaf_pending_draws = 1;
        return;
    }
    if (leaf_draw_finish_mode() == 2) {
        p_flush();
    } else {
        p_finish();
    }
    LeafGLenum err = leaf_gl_error();
    if (err != 0 && reported < 16) {
        reported++;
        LeafGLint prog = 0, vao = 0, active = 0, fbo = 0;
        LeafGLint tex[4] = {0, 0, 0, 0};
        p_getintv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
        p_getintv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &vao);
        p_getintv(0x84E0 /* GL_ACTIVE_TEXTURE */, &active);
        p_getintv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fbo);
        for (int i = 0; i < 4; i++) {
            p_activetexture(0x84C0 + i);
            p_getintv(0x8069 /* GL_TEXTURE_BINDING_2D */, &tex[i]);
        }
        p_activetexture((LeafGLenum)active);
        LEAF_GL_LOG("DRAW-FAULT frame=%lu draw=%lu %s mode=0x%x count=%d inst=%d err=0x%x prog=%d vao=%d fbo=%d tex0..3=%d,%d,%d,%d\n",
                    leaf_frame_counter, n, what, mode, count, instances, err, prog, vao, fbo,
                    tex[0], tex[1], tex[2], tex[3]);
    }
}

static void leaf_glDrawArrays(LeafGLenum mode, LeafGLint first, LeafGLsizei count)
{
    typedef void (*fn_t)(LeafGLenum, LeafGLint, LeafGLsizei);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glDrawArrays");
    }
    fn(mode, first, count);
    if (leaf_draw_finish_enabled()) {
        leaf_gl_draw_check("glDrawArrays", mode, count, 1);
    }
}

static void leaf_glDrawElements(LeafGLenum mode, LeafGLsizei count, LeafGLenum type, const void *indices)
{
    typedef void (*fn_t)(LeafGLenum, LeafGLsizei, LeafGLenum, const void *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glDrawElements");
    }
    fn(mode, count, type, indices);
    if (leaf_draw_finish_enabled()) {
        leaf_gl_draw_check("glDrawElements", mode, count, 1);
    }
}

static void leaf_glDrawArraysInstanced(LeafGLenum mode, LeafGLint first, LeafGLsizei count, LeafGLsizei instances)
{
    typedef void (*fn_t)(LeafGLenum, LeafGLint, LeafGLsizei, LeafGLsizei);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glDrawArraysInstanced");
    }
    fn(mode, first, count, instances);
    if (leaf_draw_finish_enabled()) {
        leaf_gl_draw_check("glDrawArraysInstanced", mode, count, instances);
    }
}

static void leaf_glDrawElementsInstanced(LeafGLenum mode, LeafGLsizei count, LeafGLenum type, const void *indices, LeafGLsizei instances)
{
    typedef void (*fn_t)(LeafGLenum, LeafGLsizei, LeafGLenum, const void *, LeafGLsizei);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("glDrawElementsInstanced");
    }
    fn(mode, count, type, indices, instances);
    if (leaf_draw_finish_enabled()) {
        leaf_gl_draw_check("glDrawElementsInstanced", mode, count, instances);
    }
}

static __eglMustCastToProperFunctionPointerType leaf_gl_debug_wrapper(const char *procname)
{
    if (leaf_draw_finish_mode() == 3 && strcmp(procname, "glClientWaitSync") == 0) {
        return (__eglMustCastToProperFunctionPointerType)leaf_glClientWaitSync;
    }
    if (leaf_draw_finish_mode() == 4) {
        if (strcmp(procname, "glBufferData") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glBufferData;
        }
        if (strcmp(procname, "glBufferSubData") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glBufferSubData;
        }
        if (strcmp(procname, "glTexSubImage2D") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glTexSubImage2D;
        }
        if (strcmp(procname, "glMapBufferRange") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glMapBufferRange;
        }
        if (strcmp(procname, "glDeleteBuffers") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glDeleteBuffers;
        }
        if (strcmp(procname, "glDeleteTextures") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glDeleteTextures;
        }
        if (strcmp(procname, "glDeleteVertexArrays") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glDeleteVertexArrays;
        }
    }
    if (strcmp(procname, "glBlitFramebuffer") == 0 && leaf_debug_enabled()) {
        return (__eglMustCastToProperFunctionPointerType)leaf_glBlitFramebuffer;
    }
    if (leaf_draw_finish_enabled()) {
        if (strcmp(procname, "glDrawArrays") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glDrawArrays;
        }
        if (strcmp(procname, "glDrawElements") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glDrawElements;
        }
        if (strcmp(procname, "glDrawArraysInstanced") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glDrawArraysInstanced;
        }
        if (strcmp(procname, "glDrawElementsInstanced") == 0) {
            return (__eglMustCastToProperFunctionPointerType)leaf_glDrawElementsInstanced;
        }
    }
    if (strcmp(procname, "glInvalidateFramebuffer") == 0) {
        return (__eglMustCastToProperFunctionPointerType)leaf_glInvalidateFramebuffer;
    }
    if (strcmp(procname, "glBindFramebuffer") == 0) {
        return (__eglMustCastToProperFunctionPointerType)leaf_glBindFramebuffer;
    }
    if (strcmp(procname, "glTexStorage2D") == 0) {
        return (__eglMustCastToProperFunctionPointerType)leaf_glTexStorage2D;
    }
    if (strcmp(procname, "glScissor") == 0) {
        return (__eglMustCastToProperFunctionPointerType)leaf_glScissor;
    }
    if (strcmp(procname, "glClear") == 0) {
        return (__eglMustCastToProperFunctionPointerType)leaf_glClear;
    }
    if (strcmp(procname, "glCheckFramebufferStatus") == 0) {
        return (__eglMustCastToProperFunctionPointerType)leaf_glCheckFramebufferStatus;
    }
    return NULL;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
{
    typedef __eglMustCastToProperFunctionPointerType (*fn_t)(const char *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("eglGetProcAddress");
    }

    if (leaf_debug_enabled() || leaf_draw_finish_mode() != 0) {
        __eglMustCastToProperFunctionPointerType wrapper = leaf_gl_debug_wrapper(procname);
        if (wrapper) {
            return wrapper;
        }
    }

    if (strcmp(procname, "eglGetPlatformDisplayEXT") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglGetPlatformDisplayEXT;
    }
    if (strcmp(procname, "eglCreatePlatformWindowSurfaceEXT") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglCreatePlatformWindowSurfaceEXT;
    }
    if (strcmp(procname, "eglCreatePlatformPixmapSurfaceEXT") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglCreatePlatformPixmapSurfaceEXT;
    }
    if (strcmp(procname, "eglGetPlatformDisplay") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglGetPlatformDisplay;
    }
    if (strcmp(procname, "eglCreatePlatformWindowSurface") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglCreatePlatformWindowSurface;
    }
    if (strcmp(procname, "eglCreatePlatformPixmapSurface") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglCreatePlatformPixmapSurface;
    }
    if (strcmp(procname, "eglCreateSync") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglCreateSync;
    }
    if (strcmp(procname, "eglDestroySync") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglDestroySync;
    }
    if (strcmp(procname, "eglClientWaitSync") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglClientWaitSync;
    }
    if (strcmp(procname, "eglGetSyncAttrib") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglGetSyncAttrib;
    }
    if (strcmp(procname, "eglWaitSync") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglWaitSync;
    }
    if (strcmp(procname, "eglCreateImage") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglCreateImage;
    }
    if (strcmp(procname, "eglDestroyImage") == 0) {
        return (__eglMustCastToProperFunctionPointerType)eglDestroyImage;
    }

    __eglMustCastToProperFunctionPointerType p = fn(procname);
    if (!p) {
        p = (__eglMustCastToProperFunctionPointerType)leaf_sym(procname);
    }
    return p;
}
