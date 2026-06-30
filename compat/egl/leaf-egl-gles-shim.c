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
        leaf_mali = dlopen("/lib/libmali.so.1", RTLD_LAZY | RTLD_GLOBAL);
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
LEAF_EGL_FWD(EGLSurface, eglCreateWindowSurface, (EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list), (dpy, config, win, attrib_list))
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
LEAF_EGL_FWD(EGLBoolean, eglSwapBuffers, (EGLDisplay dpy, EGLSurface surface), (dpy, surface))
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

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
{
    typedef __eglMustCastToProperFunctionPointerType (*fn_t)(const char *);
    static fn_t fn;
    if (!fn) {
        fn = (fn_t)leaf_sym("eglGetProcAddress");
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
