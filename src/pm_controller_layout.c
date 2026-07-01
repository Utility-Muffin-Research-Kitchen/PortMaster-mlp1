#include "pm_controller_layout.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *PM_MLP1_GAMECONTROLLERCONFIG_GUI =
    "1900fe3c039900001399000002010000,Loong Gamepad,"
    "a:b1,b:b0,x:b3,y:b2,back:b8,guide:b10,start:b9,"
    "leftstick:b11,leftshoulder:b4,rightshoulder:b5,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "leftx:a0,lefty:a1,lefttrigger:b6,righttrigger:b7,"
    "crc:3cfe,platform:Linux";

static int sentinel_path(const pm_context *ctx, char *path, size_t path_size)
{
    if (!ctx) {
        return -1;
    }
    return pm_join(path, path_size, ctx->data_dir, "nintendo");
}

static void set_err(char *err, size_t err_size, const char *fmt, ...)
{
    if (!err || err_size == 0 || !fmt) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

const char *pm_controller_layout_slug(pm_controller_layout layout)
{
    return layout == PM_CONTROLLER_LAYOUT_NINTENDO ? "nintendo" : "x360";
}

const char *pm_controller_layout_label(pm_controller_layout layout)
{
    return layout == PM_CONTROLLER_LAYOUT_NINTENDO ? "Nintendo" : "X360";
}

const char *pm_controller_layout_gui_sdl_config(void)
{
    return PM_MLP1_GAMECONTROLLERCONFIG_GUI;
}

int pm_controller_layout_from_string(const char *value, pm_controller_layout *out)
{
    if (!value || !out) {
        return -1;
    }
    if (strcasecmp(value, "nintendo") == 0) {
        *out = PM_CONTROLLER_LAYOUT_NINTENDO;
        return 0;
    }
    if (strcasecmp(value, "x360") == 0 || strcasecmp(value, "xbox") == 0) {
        *out = PM_CONTROLLER_LAYOUT_X360;
        return 0;
    }
    return -1;
}

int pm_controller_layout_load(const pm_context *ctx, pm_controller_layout *out)
{
    char path[PM_PATH_MAX];
    if (!out) {
        return -1;
    }
    *out = PM_CONTROLLER_LAYOUT_X360;
    if (sentinel_path(ctx, path, sizeof(path)) != 0) {
        return -1;
    }
    if (pm_file_exists(path)) {
        *out = PM_CONTROLLER_LAYOUT_NINTENDO;
    }
    return 0;
}

int pm_controller_layout_save(const pm_context *ctx, pm_controller_layout layout,
                              char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx) {
        set_err(err, err_size, "missing PortMaster context");
        return -1;
    }
    if (pm_mkdir_p(ctx->data_dir, err, err_size) != 0) {
        return -1;
    }

    char path[PM_PATH_MAX];
    if (sentinel_path(ctx, path, sizeof(path)) != 0) {
        set_err(err, err_size, "controller layout path too long");
        return -1;
    }

    if (layout == PM_CONTROLLER_LAYOUT_NINTENDO) {
        FILE *fp = fopen(path, "wb");
        if (!fp) {
            set_err(err, err_size, "cannot write %s: %s", path, strerror(errno));
            return -1;
        }
        if (fputs("nintendo\n", fp) < 0 || fclose(fp) != 0) {
            set_err(err, err_size, "cannot finish %s: %s", path, strerror(errno));
            return -1;
        }
    } else {
        if (unlink(path) != 0 && errno != ENOENT) {
            set_err(err, err_size, "cannot remove %s: %s", path, strerror(errno));
            return -1;
        }
    }

    return pm_controller_layout_sync_hook(ctx, err, err_size);
}

int pm_controller_layout_sync_hook(const pm_context *ctx, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx) {
        set_err(err, err_size, "missing PortMaster context");
        return -1;
    }
    if (!pm_dir_exists(ctx->portmaster_dir)) {
        return 0;
    }

    char script[PM_PATH_MAX];
    char hook_path[PM_PATH_MAX];
    if (pm_join3(script, sizeof(script), ctx->pak_dir,
                 "scripts", "write-leaf-runtime-hook.sh") != 0 ||
        pm_join(hook_path, sizeof(hook_path), ctx->portmaster_dir,
                "leaf-armhf-env.sh") != 0) {
        set_err(err, err_size, "controller hook path too long");
        return -1;
    }
    if (!pm_file_exists(script)) {
        set_err(err, err_size, "runtime hook writer missing: %s", script);
        return -1;
    }

    pm_env_override env[] = {
        { "PLATFORM", ctx->platform },
        { "SDCARD_PATH", ctx->sdcard_path },
        { "USERDATA_PATH", ctx->userdata_path },
        { "ROMS_PATH", ctx->roms_path },
        { "PORTMASTER_MLP1_DATA_DIR", ctx->data_dir },
        { "PORTMASTER_CONTROLFOLDER", ctx->portmaster_dir },
        { NULL, NULL },
    };
    char *argv[] = { "bash", script, hook_path, NULL };
    char run_err[512];
    if (pm_run_argv_env_in_dir(ctx->pak_dir, argv, env, run_err, sizeof(run_err)) != 0) {
        set_err(err, err_size, "controller hook sync failed: %s",
                run_err[0] ? run_err : "unknown error");
        return -1;
    }
    return 0;
}
