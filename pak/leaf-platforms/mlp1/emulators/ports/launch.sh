#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd "$(dirname "$0")" && pwd)"
PLATFORM_ROOT="$(CDPATH= cd "$SCRIPT_DIR/../.." && pwd)"
retroarch_wrapper_dir=""
retroarch_wrapper=""
retroarch_compat_wrapper=""
ports_bind_mounted=0
port_pid=""
port_uses_setsid=0

cleanup() {
    if [ -n "$port_pid" ] && kill -0 "$port_pid" 2>/dev/null; then
        if [ "$port_uses_setsid" = "1" ]; then
            kill -TERM "-$port_pid" 2>/dev/null || kill -TERM "$port_pid" 2>/dev/null || true
            sleep 1
            kill -KILL "-$port_pid" 2>/dev/null || kill -KILL "$port_pid" 2>/dev/null || true
        else
            kill -TERM "$port_pid" 2>/dev/null || true
            sleep 1
            kill -KILL "$port_pid" 2>/dev/null || true
        fi
    fi
    # The PortMaster launcher must not create/remove files under /usr or other
    # stock rootfs paths. RetroArch shims are scoped to /tmp and PATH only.
    if [ -n "$retroarch_compat_wrapper" ]; then
        rm -f "$retroarch_compat_wrapper" 2>/dev/null || true
    fi
    if [ -n "$retroarch_wrapper" ]; then
        rm -f "$retroarch_wrapper" 2>/dev/null || true
    fi
    if [ -n "$retroarch_wrapper_dir" ]; then
        rmdir "$retroarch_wrapper_dir" 2>/dev/null || true
    fi
    if [ "$ports_bind_mounted" = "1" ]; then
        umount /roms/ports 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

if [ -f "$PLATFORM_ROOT/launcher/env.sh" ]; then
    . "$PLATFORM_ROOT/launcher/env.sh"
fi

resolve_mlp1_virtual_gamepad() {
    awk '
        /^N: Name="Loong Gamepad"/ {
            name = 1
        }
        /^S: Sysfs=\/devices\/virtual\/input\// {
            virtual = 1
        }
        /^H: Handlers=/ {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^event[0-9]+$/) {
                    event = $i
                }
            }
        }
        name && virtual && event != "" {
            print "/dev/input/" event
            exit
        }
    ' /proc/bus/input/devices 2>/dev/null
}

find_optional_portmaster_runtime_prepare() {
    if [ -n "${PORTMASTER_RUNTIME_PREPARE:-}" ]; then
        [ -x "$PORTMASTER_RUNTIME_PREPARE" ] && printf '%s\n' "$PORTMASTER_RUNTIME_PREPARE"
        return 0
    fi

    app_roots="${APPS_PATHS:-}"
    if [ -z "$app_roots" ]; then
        app_roots="${APPS_PATH:-${SDCARD_PATH:-/mnt/sdcard}/Apps}"
    fi

    old_ifs="$IFS"
    IFS=:
    set -- $app_roots
    IFS="$old_ifs"

    for app_root in "$@"; do
        [ -n "$app_root" ] || continue
        for rel in \
            "${PLATFORM:-mlp1}/PortMaster.pak/scripts/prepare-port-runtime.sh" \
            "shared/PortMaster.pak/scripts/prepare-port-runtime.sh"; do
            candidate="$app_root/$rel"
            if [ -x "$candidate" ]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done
    done

    return 0
}

run_optional_portmaster_runtime_prepare() {
    [ "${LEAF_PM_PORT_RUNTIME:-1}" = "0" ] && return 0

    prepare_script="$(find_optional_portmaster_runtime_prepare | head -n 1)"
    [ -n "$prepare_script" ] || return 0

    echo "[ports] preparing optional PortMaster runtime"
    PORTMASTER_PORT_SCRIPT="$port_script" \
    PORTMASTER_PORTS_DIR="$ports_dir" \
    PORTMASTER_MLP1_DATA_DIR="$pm_data" \
    "$prepare_script" || echo "[ports] optional PortMaster runtime prep failed: $prepare_script"
}

port_script="${1:-${JAWAKA_GAME_ROM_ABS:-}}"
if [ -z "$port_script" ]; then
    echo "ports launcher: missing port script path" >&2
    exit 64
fi

case "$port_script" in
    /*) ;;
    *) port_script="${SDCARD_PATH:-/mnt/sdcard}/$port_script" ;;
esac

if [ ! -f "$port_script" ]; then
    echo "ports launcher: script not found: $port_script" >&2
    exit 66
fi

port_shell="${PORTMASTER_BASH:-}"
if [ -z "$port_shell" ]; then
    port_shell="$(command -v bash 2>/dev/null || true)"
fi
if [ -z "$port_shell" ]; then
    port_shell="/usr/bin/bash"
fi
if [ ! -x "$port_shell" ]; then
    echo "ports launcher: bash not found: $port_shell" >&2
    exit 69
fi

case "$port_script" in
    */Roms/PORTS/*.sh|*/Roms/Ports/*.sh|*/Roms/ports/*.sh) ;;
    *)
        echo "ports launcher: refusing non-PortMaster script: $port_script" >&2
        exit 65
        ;;
esac

pm_data="${USERDATA_PATH:-${SDCARD_PATH:-/mnt/sdcard}/.userdata/${PLATFORM:-mlp1}}/portmaster"
ports_dir="$(CDPATH= cd "$(dirname "$port_script")" && pwd)"
roms_dir="$(CDPATH= cd "$ports_dir/.." && pwd)"
export HOME="$pm_data"
export XDG_DATA_HOME="$pm_data"
export PORTMASTER_CONTROLFOLDER="${PORTMASTER_CONTROLFOLDER:-$pm_data/PortMaster}"
export PORTMASTER_LEAF_PORT_LAYOUT_SCOPE=ports
export PORTMASTER_ROMS_DIRECTORY="${PORTMASTER_ROMS_DIRECTORY:-${roms_dir#/}}"
export PORTMASTER_LEAF_DEVICE_INFO=1
export CFW_NAME="${CFW_NAME:-Leaf}"
export DEVICE_NAME="${DEVICE_NAME:-Miniloong Pocket 1}"
export DEVICE_CPU="${DEVICE_CPU:-RK3566}"
export DEVICE_ARCH="${DEVICE_ARCH:-aarch64}"
export DEVICE_HAS_ARMHF="${DEVICE_HAS_ARMHF:-N}"
export DEVICE_HAS_AARCH64="${DEVICE_HAS_AARCH64:-Y}"
export DEVICE_HAS_X86="${DEVICE_HAS_X86:-N}"
export DEVICE_HAS_X86_64="${DEVICE_HAS_X86_64:-N}"
export DISPLAY_WIDTH="${DISPLAY_WIDTH:-960}"
export DISPLAY_HEIGHT="${DISPLAY_HEIGHT:-720}"
export DISPLAY_ORIENTATION="${DISPLAY_ORIENTATION:-0}"
export ASPECT_X="${ASPECT_X:-4}"
export ASPECT_Y="${ASPECT_Y:-3}"
export ANALOG_STICKS="${ANALOG_STICKS:-2}"
export ANALOGSTICKS="${ANALOGSTICKS:-2}"
export UMRK_RETROARCH_BIN="${UMRK_RETROARCH_BIN:-$PLATFORM_ROOT/bin/retroarch}"
export UMRK_RETROARCH_CONFIG="$pm_data/.config/retroarch/retroarch.cfg"

case "${JAWAKA_DIRECT_DRM:-0}" in
    1|true|yes|TRUE|YES)
        export LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE="${LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE:-1}"
        export LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE_STOP_DISPLAY="${LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE_STOP_DISPLAY:-0}"
        ;;
esac

run_optional_portmaster_runtime_prepare

if [ "${PLATFORM:-mlp1}" = "mlp1" ] && [ -z "${SDL_JOYSTICK_DEVICE:-}" ]; then
    if [ -n "${JAWAKA_INPUT_VIRTUAL_EVENT:-}" ] &&
       [ -e "$JAWAKA_INPUT_VIRTUAL_EVENT" ]; then
        MLP1_VIRTUAL_GAMEPAD="$JAWAKA_INPUT_VIRTUAL_EVENT"
    elif [ -n "${JAWAKA_RETROARCH_VIRTUAL_EVENT:-}" ] &&
         [ -e "$JAWAKA_RETROARCH_VIRTUAL_EVENT" ]; then
        MLP1_VIRTUAL_GAMEPAD="$JAWAKA_RETROARCH_VIRTUAL_EVENT"
    else
        MLP1_VIRTUAL_GAMEPAD="$(resolve_mlp1_virtual_gamepad || true)"
    fi

    if [ -n "$MLP1_VIRTUAL_GAMEPAD" ] && [ -e "$MLP1_VIRTUAL_GAMEPAD" ]; then
        export SDL_JOYSTICK_DEVICE="$MLP1_VIRTUAL_GAMEPAD"
        echo "[ports] using calibrated Jawaka virtual gamepad: $SDL_JOYSTICK_DEVICE"
    else
        echo "[ports] calibrated Jawaka virtual gamepad not found; using SDL default joystick scan"
    fi
fi

write_retroarch_config() {
    mkdir -p "$pm_data/.config/retroarch" "$pm_data/BIOS" "$pm_data/saves" \
        "$pm_data/states" "$pm_data/logs"

    tmp_config="$UMRK_RETROARCH_CONFIG.tmp.$$"
    if [ -f "$PLATFORM_ROOT/defaults/retroarch.cfg" ]; then
        cp "$PLATFORM_ROOT/defaults/retroarch.cfg" "$tmp_config"
    else
        {
            printf '%s\n' 'config_save_on_exit = "false"'
            printf '%s\n' 'video_driver = "gl"'
            printf '%s\n' 'video_context_driver = "sdl_gl"'
            printf '%s\n' 'audio_driver = "pulse"'
            printf '%s\n' 'input_driver = "sdl2"'
            printf '%s\n' 'input_joypad_driver = "sdl2"'
            printf '%s\n' 'menu_driver = "rgui"'
            printf '%s\n' 'video_fullscreen = "true"'
            printf '%s\n' 'pause_nonactive = "false"'
        } >"$tmp_config"
    fi

    {
        printf '\n'
        printf 'system_directory = "%s/BIOS"\n' "$pm_data"
        printf 'savefile_directory = "%s/saves"\n' "$pm_data"
        printf 'savestate_directory = "%s/states"\n' "$pm_data"
        printf 'libretro_directory = "%s/cores"\n' "$PLATFORM_ROOT"
        printf 'libretro_info_path = "%s/info"\n' "$PLATFORM_ROOT"
        printf 'rgui_browser_directory = "%s"\n' "$ports_dir"
        printf '%s\n' 'config_save_on_exit = "false"'
        printf '%s\n' 'pause_nonactive = "false"'
        printf '%s\n' 'check_firmware_before_loading = "false"'
        printf '%s\n' 'load_dummy_on_core_shutdown = "false"'
    } >>"$tmp_config"

    mv "$tmp_config" "$UMRK_RETROARCH_CONFIG"
}

write_retroarch_wrapper() {
    wrapper_path="$1"
    mkdir -p "$(dirname "$wrapper_path")"
    cat >"$wrapper_path" <<'SH'
#!/bin/sh
set -eu

real_ra="${UMRK_RETROARCH_BIN:?}"
config="${UMRK_RETROARCH_CONFIG:?}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}"

has_config=0
for arg in "$@"; do
    if [ "$arg" = "--config" ] || [ "$arg" = "-c" ]; then
        has_config=1
    fi
done

if [ "$has_config" = "1" ]; then
    exec "$real_ra" "$@"
fi

if [ "${UMRK_RETROARCH_VERBOSE:-0}" = "1" ]; then
    exec "$real_ra" --verbose --config "$config" "$@"
fi

exec "$real_ra" --config "$config" "$@"
SH
    chmod 755 "$wrapper_path"
}

if [ ! -x "$UMRK_RETROARCH_BIN" ]; then
    echo "ports launcher: RetroArch missing: $UMRK_RETROARCH_BIN" >&2
    exit 69
fi

write_retroarch_config
retroarch_wrapper_dir="/tmp/leaf-portmaster-retroarch-bin.$$"
retroarch_wrapper="$retroarch_wrapper_dir/retroarch"
write_retroarch_wrapper "$retroarch_wrapper"
export PATH="$retroarch_wrapper_dir:${PATH:-/usr/bin:/usr/sbin:/bin:/sbin}"

if [ -L /usr/bin/retroarch ]; then
    retroarch_target="$(readlink /usr/bin/retroarch 2>/dev/null || true)"
    case "$retroarch_target" in
        /tmp/leaf-portmaster-retroarch.*)
            if [ ! -e "$retroarch_target" ]; then
                retroarch_compat_wrapper="$retroarch_target"
                write_retroarch_wrapper "$retroarch_compat_wrapper"
            fi
            ;;
    esac
fi

if [ -d "$ports_dir" ] && [ -d /roms/ports ] &&
   ! awk '$2 == "/roms/ports" { found = 1 } END { exit found ? 0 : 1 }' /proc/mounts; then
    if mount --bind "$ports_dir" /roms/ports 2>/dev/null; then
        ports_bind_mounted=1
    fi
elif [ -d "$ports_dir" ] && [ ! -d /roms/ports ]; then
    echo "ports launcher: /roms/ports mountpoint absent; not creating rootfs paths" >&2
fi

cd "$ports_dir"
if command -v setsid >/dev/null 2>&1; then
    setsid "$port_shell" "$port_script" &
    port_uses_setsid=1
else
    "$port_shell" "$port_script" &
fi
port_pid="$!"
set +e
wait "$port_pid"
status="$?"
set -e
port_pid=""
exit "$status"
