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

    int ok = 1;
    ok = ok && fprintf(fp,
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
        "export PORTMASTER_CONTROLFOLDER=\"${PORTMASTER_CONTROLFOLDER:-$_leaf_pm_controlfolder}\"\n"
        "export HOME=\"$LEAF_PM_DATA_DIR\"\n"
        "export XDG_DATA_HOME=\"$LEAF_PM_DATA_DIR\"\n"
        "export HM_TOOLS_DIR=\"${HM_TOOLS_DIR:-$LEAF_PM_DATA_DIR}\"\n"
        "if [ -n \"${ROMS_PATH:-}\" ]; then\n"
        "  _leaf_pm_roms_dir=\"${ROMS_PATH%%/}\"\n"
        "  _leaf_pm_ports_dir=\"$ROMS_PATH/PORTS\"\n"
        "elif [ -n \"${SDCARD_PATH:-}\" ]; then\n"
        "  _leaf_pm_roms_dir=\"${SDCARD_PATH%%/}/Roms\"\n"
        "  _leaf_pm_ports_dir=\"$SDCARD_PATH/Roms/PORTS\"\n"
        "else\n"
        "  _leaf_pm_roms_dir=\"\"\n"
        "  _leaf_pm_ports_dir=\"/${directory:-roms}/ports\"\n"
        "fi\n"
        "if [ -n \"$_leaf_pm_roms_dir\" ] &&\n"
        "   ! awk '$2 == \"/roms/ports\" { found = 1 } END { exit found ? 0 : 1 }' /proc/mounts 2>/dev/null; then\n"
        "  export directory=\"${_leaf_pm_roms_dir#/}\"\n"
        "fi\n"
        "export HM_PORTS_DIR=\"${HM_PORTS_DIR:-$_leaf_pm_ports_dir}\"\n"
        "export HM_SCRIPTS_DIR=\"${HM_SCRIPTS_DIR:-$HM_PORTS_DIR}\"\n"
        "\n"
        "export LEAF_PM_RUNTIME_DIR=\"${LEAF_PM_RUNTIME_DIR:-$LEAF_PM_DATA_DIR/runtime}\"\n"
        "if [ -x \"$LEAF_PM_RUNTIME_DIR/bin/python3\" ]; then\n"
        "  _leaf_pm_python_shim_dir=\"/tmp/leaf-portmaster-python\"\n"
        "  if mkdir -p \"$_leaf_pm_python_shim_dir\" 2>/dev/null; then\n"
        "    cat >\"$_leaf_pm_python_shim_dir/python3\" <<'LEAF_PM_PYTHON_SHIM'\n"
        "#!/bin/sh\n"
        "_runtime=\"${LEAF_PM_RUNTIME_DIR:-}\"\n"
        "if [ -z \"$_runtime\" ] || [ ! -x \"$_runtime/bin/python3\" ]; then\n"
        "  echo \"Leaf PortMaster python runtime missing\" >&2\n"
        "  exit 127\n"
        "fi\n"
        "if [ -n \"${LD_LIBRARY_PATH:-}\" ]; then\n"
        "  export LD_LIBRARY_PATH=\"$_runtime/lib:$LD_LIBRARY_PATH\"\n"
        "else\n"
        "  export LD_LIBRARY_PATH=\"$_runtime/lib\"\n"
        "fi\n"
        "export PYTHONHOME=\"$_runtime\"\n"
        "export PYTHONPATH=\"$_runtime/lib/python3.10:$_runtime/lib/python3.10/site-packages:$_runtime/lib${PYTHONPATH:+:$PYTHONPATH}\"\n"
        "export PYTHONDONTWRITEBYTECODE=\"${PYTHONDONTWRITEBYTECODE:-1}\"\n"
        "exec \"$_runtime/bin/python3\" \"$@\"\n"
        "LEAF_PM_PYTHON_SHIM\n"
        "    chmod 755 \"$_leaf_pm_python_shim_dir/python3\" 2>/dev/null || true\n"
        "    cp -f \"$_leaf_pm_python_shim_dir/python3\" \"$_leaf_pm_python_shim_dir/python\" 2>/dev/null || true\n"
        "    case \":${PATH:-}:\" in\n"
        "      *:\"$_leaf_pm_python_shim_dir\":*) ;;\n"
        "      *) export PATH=\"$_leaf_pm_python_shim_dir:${PATH:-/usr/bin:/usr/sbin}\" ;;\n"
        "    esac\n"
        "  fi\n"
        "fi\n"
        "\n",
        ctx->portmaster_dir,
        ctx->data_dir) > 0;

    ok = ok && fputs(
        "export LEAF_PM_EGL_SHIM_DIR=\"${LEAF_PM_EGL_SHIM_DIR:-$LEAF_PM_DATA_DIR/compat/egl/aarch64}\"\n"
        "export LEAF_PM_MALI_AARCH64_DIR=\"${LEAF_PM_MALI_AARCH64_DIR:-$LEAF_PM_DATA_DIR/compat/mali/aarch64}\"\n"
        "leaf_pm_enable_godot_wayland_runtime() {\n"
        "  [ \"${DEVICE_ARCH:-aarch64}\" = \"aarch64\" ] || return 0\n"
        "  export LEAF_PM_SKIP_WESTONPACK_CLEANUP=\"${LEAF_PM_SKIP_WESTONPACK_CLEANUP:-1}\"\n"
        "  _leaf_pm_env_bin=\"$(command -v env 2>/dev/null || printf '%s\\n' /usr/bin/env)\"\n"
        "  env() {\n"
        "    if [ \"${1##*/}\" = \"westonwrap.sh\" ] && [ \"${2:-}\" != \"cleanup\" ] && [ \"$#\" -ge 6 ]; then\n"
        "      # MLP1 already has Leaf's Weston running; PortMaster's nested Westonpack\n"
        "      # wrapper cannot provide the EGL display Godot 4 expects on stock firmware.\n"
        "      shift\n"
        "      shift 4\n"
        "      while [ \"$#\" -gt 0 ]; do\n"
        "        case \"$1\" in\n"
        "          *=*) export \"$1\"; shift ;;\n"
        "          *) break ;;\n"
        "        esac\n"
        "      done\n"
        "      [ \"$#\" -gt 0 ] || return 127\n"
        "      _leaf_pm_godot_cmd=\"$1\"\n"
        "      shift\n"
        "      _leaf_pm_wayland_runtime=\"${LEAF_PM_WAYLAND_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-/run}}\"\n"
        "      _leaf_pm_wayland_display=\"${LEAF_PM_WAYLAND_DISPLAY:-${WAYLAND_DISPLAY:-wayland-0}}\"\n"
        "      _leaf_pm_egl_shim=\"${LEAF_PM_EGL_SHIM_DIR:-}/libEGL.so.1\"\n"
        "      if [ -f \"${LEAF_PM_MALI_AARCH64_DIR:-}/libmali.so.1\" ]; then\n"
        "        case \":${LD_LIBRARY_PATH:-}:\" in\n"
        "          *:\"$LEAF_PM_MALI_AARCH64_DIR\":*) ;;\n"
        "          *) export LD_LIBRARY_PATH=\"$LEAF_PM_MALI_AARCH64_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\" ;;\n"
        "        esac\n"
        "      fi\n"
        "      if [ -f \"$_leaf_pm_egl_shim\" ]; then\n"
        "        case \":${LD_LIBRARY_PATH:-}:\" in\n"
        "          *:\"$LEAF_PM_EGL_SHIM_DIR\":*) ;;\n"
        "          *) export LD_LIBRARY_PATH=\"$LEAF_PM_EGL_SHIM_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\" ;;\n"
        "        esac\n"
        "      fi\n"
        "      _leaf_pm_has_display_driver=0\n"
        "      for _leaf_pm_arg in \"$@\"; do\n"
        "        [ \"$_leaf_pm_arg\" = \"--display-driver\" ] && _leaf_pm_has_display_driver=1\n"
        "      done\n"
        "      if [ \"$_leaf_pm_has_display_driver\" -eq 1 ]; then\n"
        "        \"$_leaf_pm_env_bin\" \\\n"
        "          XDG_RUNTIME_DIR=\"$_leaf_pm_wayland_runtime\" \\\n"
        "          WAYLAND_DISPLAY=\"$_leaf_pm_wayland_display\" \\\n"
        "          SDL_VIDEODRIVER=wayland \\\n"
        "          \"$_leaf_pm_godot_cmd\" \"$@\"\n"
        "        return $?\n"
        "      fi\n"
        "      \"$_leaf_pm_env_bin\" \\\n"
        "        XDG_RUNTIME_DIR=\"$_leaf_pm_wayland_runtime\" \\\n"
        "        WAYLAND_DISPLAY=\"$_leaf_pm_wayland_display\" \\\n"
        "        SDL_VIDEODRIVER=wayland \\\n"
        "        \"$_leaf_pm_godot_cmd\" --display-driver wayland \"$@\"\n"
        "      return $?\n"
        "    fi\n"
        "    \"$_leaf_pm_env_bin\" \"$@\"\n"
        "  }\n"
        "}\n"
        "\n"
        "leaf_pm_enable_egl_gles_shim() {\n"
        "  leaf_pm_enable_godot_wayland_runtime \"$@\"\n"
        "}\n"
        "\n",
        fp) >= 0;

    ok = ok && fprintf(fp,
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
        "}\n",
        PM_MLP1_GAMECONTROLLERCONFIG_GUI,
        PM_MLP1_GAMECONTROLLERCONFIG_NINTENDO,
        PM_MLP1_GAMECONTROLLERCONFIG_X360) > 0;

    ok = ok && fputs(
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
        "\n"
        "  leaf_pm_armhf_run() {\n"
        "    \"$LEAF_PM_ARMHF_RUN\" \"$@\"\n"
        "  }\n"
        "fi\n"
        "\n"
        "unset _leaf_pm_controlfolder _leaf_pm_data_dir _leaf_pm_roms_dir _leaf_pm_ports_dir _leaf_pm_python_shim_dir\n",
        fp) >= 0;

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
