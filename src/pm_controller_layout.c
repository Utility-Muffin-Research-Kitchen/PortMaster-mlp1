#include "pm_controller_layout.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *PM_MLP1_GAMECONTROLLERCONFIG_X360 =
    "1900fe3c039900001399000002010000,Loong Gamepad,"
    "a:b0,b:b1,x:b3,y:b2,back:b8,guide:b10,start:b9,"
    "leftstick:b11,leftshoulder:b4,rightshoulder:b5,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "leftx:a0,lefty:a1,lefttrigger:b6,righttrigger:b7,"
    "crc:3cfe,platform:Linux";

static const char *PM_MLP1_GAMECONTROLLERCONFIG_GUI =
    "1900fe3c039900001399000002010000,Loong Gamepad,"
    "a:b1,b:b0,x:b3,y:b2,back:b8,guide:b10,start:b9,"
    "leftstick:b11,leftshoulder:b4,rightshoulder:b5,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "leftx:a0,lefty:a1,lefttrigger:b6,righttrigger:b7,"
    "crc:3cfe,platform:Linux";

static const char *PM_MLP1_GAMECONTROLLERCONFIG_NINTENDO =
    "1900fe3c039900001399000002010000,Loong Gamepad,"
    "a:b1,b:b0,x:b2,y:b3,back:b8,guide:b10,start:b9,"
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

const char *pm_controller_layout_sdl_config(pm_controller_layout layout)
{
    return layout == PM_CONTROLLER_LAYOUT_NINTENDO
        ? PM_MLP1_GAMECONTROLLERCONFIG_NINTENDO
        : PM_MLP1_GAMECONTROLLERCONFIG_X360;
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

    char hook_path[PM_PATH_MAX];
    char tmp_path[PM_PATH_MAX];
    if (pm_join(hook_path, sizeof(hook_path), ctx->portmaster_dir, "leaf-armhf-env.sh") != 0 ||
        pm_format(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", hook_path, (long)getpid()) != 0) {
        set_err(err, err_size, "controller hook path too long");
        return -1;
    }

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        set_err(err, err_size, "cannot write %s: %s", tmp_path, strerror(errno));
        return -1;
    }

    int ok = fprintf(fp,
        "#!/bin/sh\n"
        "# Generated by PortMaster-mlp1. Safe to overwrite.\n"
        "\n"
        "_leaf_pm_controlfolder=\"${controlfolder:-${PORTMASTER_CONTROLFOLDER:-%s}}\"\n"
        "if [ -n \"$_leaf_pm_controlfolder\" ] && [ -d \"$_leaf_pm_controlfolder/..\" ]; then\n"
        "  _leaf_pm_data_dir=\"$(CDPATH= cd -- \"$_leaf_pm_controlfolder/..\" && pwd)\"\n"
        "else\n"
        "  _leaf_pm_data_dir=\"%s\"\n"
        "fi\n"
        "export LEAF_PM_DATA_DIR=\"$_leaf_pm_data_dir\"\n"
        "\n"
        "leaf_pm_apply_controller_layout() {\n"
        "  if [ \"${PORTMASTER_LEAF_PORT_LAYOUT_SCOPE:-ports}\" = \"gui\" ]; then\n"
        "    export PORTMASTER_LEAF_CONTROLLER_LAYOUT=\"gui\"\n"
        "    export SDL_GAMECONTROLLERCONFIG='%s'\n"
        "  elif [ -f \"$LEAF_PM_DATA_DIR/nintendo\" ]; then\n"
        "    export PORTMASTER_LEAF_CONTROLLER_LAYOUT=\"nintendo\"\n"
        "    export SDL_GAMECONTROLLERCONFIG='%s'\n"
        "  else\n"
        "    export PORTMASTER_LEAF_CONTROLLER_LAYOUT=\"x360\"\n"
        "    export SDL_GAMECONTROLLERCONFIG='%s'\n"
        "  fi\n"
        "  export sdl_controllerconfig=\"$SDL_GAMECONTROLLERCONFIG\"\n"
        "  export SDL_GAMECONTROLLERCONFIG_FILE=\"/tmp/leaf-portmaster-gamecontrollerdb.txt\"\n"
        "  printf '%%s\\n' \"$SDL_GAMECONTROLLERCONFIG\" >\"$SDL_GAMECONTROLLERCONFIG_FILE\" 2>/dev/null || true\n"
        "}\n"
        "\n"
        "if declare -f get_controls >/dev/null 2>&1; then\n"
        "  if ! declare -f leaf_pm_upstream_get_controls >/dev/null 2>&1; then\n"
        "    eval \"$(declare -f get_controls | sed '1s/get_controls/leaf_pm_upstream_get_controls/')\"\n"
        "  fi\n"
        "  get_controls() {\n"
        "    leaf_pm_upstream_get_controls \"$@\"\n"
        "    leaf_pm_apply_controller_layout\n"
        "  }\n"
        "fi\n"
        "\n"
        "leaf_pm_apply_controller_layout\n"
        "\n"
        "export DEVICE_HAS_ARMHF=\"${DEVICE_HAS_ARMHF:-N}\"\n"
        "export DEVICE_HAS_AARCH64=\"${DEVICE_HAS_AARCH64:-Y}\"\n"
        "\n"
        "export LEAF_PM_ARMHF_ROOT=\"${LEAF_PM_ARMHF_ROOT:-$LEAF_PM_DATA_DIR/compat/armhf}\"\n"
        "if [ -f \"$LEAF_PM_ARMHF_ROOT/lib/ld-linux-armhf.so.3\" ] && [ -f \"$LEAF_PM_ARMHF_ROOT/bin/leaf-armhf-run\" ]; then\n"
        "  export DEVICE_HAS_ARMHF=\"Y\"\n"
        "  export LEAF_PM_ARMHF_LOADER=\"$LEAF_PM_ARMHF_ROOT/lib/ld-linux-armhf.so.3\"\n"
        "  export LEAF_PM_ARMHF_RUN=\"$LEAF_PM_ARMHF_ROOT/bin/leaf-armhf-run\"\n"
        "  export LEAF_PM_ARMHF_LIB_PATH=\"$LEAF_PM_ARMHF_ROOT/usr/lib/arm-linux-gnueabihf/mali:$LEAF_PM_ARMHF_ROOT/lib/arm-linux-gnueabihf:$LEAF_PM_ARMHF_ROOT/usr/lib/arm-linux-gnueabihf:$LEAF_PM_ARMHF_ROOT/usr/lib/arm-linux-gnueabihf/pulseaudio:$LEAF_PM_ARMHF_ROOT/lib:$LEAF_PM_ARMHF_ROOT/usr/lib\"\n"
        "  export LIBGL_DRIVERS_PATH=\"${LIBGL_DRIVERS_PATH:-$LEAF_PM_ARMHF_ROOT/usr/lib/arm-linux-gnueabihf/dri}\"\n"
        "  export __EGL_VENDOR_LIBRARY_DIRS=\"${__EGL_VENDOR_LIBRARY_DIRS:-$LEAF_PM_ARMHF_ROOT/usr/share/glvnd/egl_vendor.d}\"\n"
        "  export PULSE_SERVER=\"${PULSE_SERVER:-unix:/tmp/pulse-socket}\"\n"
        "  export PULSE_CLIENTCONFIG=\"${PULSE_CLIENTCONFIG:-$LEAF_PM_ARMHF_ROOT/etc/pulse/client.conf}\"\n"
        "  export ALSOFT_DRIVERS=\"${ALSOFT_DRIVERS:-pulse}\"\n"
        "  export ALSOFT_CONF=\"${ALSOFT_CONF:-$LEAF_PM_ARMHF_ROOT/etc/openal/alsoft.conf}\"\n"
        "\n"
        "  leaf_pm_armhf_run() {\n"
        "    \"$LEAF_PM_ARMHF_RUN\" \"$@\"\n"
        "  }\n"
        "fi\n"
        "\n"
        "unset _leaf_pm_controlfolder _leaf_pm_data_dir\n",
        ctx->portmaster_dir,
        ctx->data_dir,
        PM_MLP1_GAMECONTROLLERCONFIG_GUI,
        PM_MLP1_GAMECONTROLLERCONFIG_NINTENDO,
        PM_MLP1_GAMECONTROLLERCONFIG_X360) > 0;

    if (fclose(fp) != 0 || !ok) {
        unlink(tmp_path);
        set_err(err, err_size, "cannot finish %s: %s", tmp_path, strerror(errno));
        return -1;
    }
    if (rename(tmp_path, hook_path) != 0) {
        unlink(tmp_path);
        set_err(err, err_size, "cannot replace %s: %s", hook_path, strerror(errno));
        return -1;
    }
    chmod(hook_path, 0755);
    return 0;
}
