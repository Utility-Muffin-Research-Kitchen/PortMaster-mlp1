#!/usr/bin/env bash
# Host checks for the runtime hook's launch-time behavior: the files the writer
# installs next to it, the first-source-only HOME/XDG_DATA_HOME rule, the
# 32-bit Westonpack adjustments, mod_Leaf.txt, leaf-gptokeyb-run, the SDL3-on-SDL2
# direct-DRM path, clean init-service restarts, and the
# hook refresh in prepare-port-runtime.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-runtime-hook.XXXXXX")"
# The hook resolves its data folder with cd/pwd; match that spelling.
TMP_ROOT="$(cd "$TMP_ROOT" && pwd)"
SOCKET_PID=""
cleanup() {
  [ -n "$SOCKET_PID" ] && kill "$SOCKET_PID" 2>/dev/null || true
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

failures=0
check() {
  local what="$1"
  shift
  if "$@"; then
    printf 'ok   %s\n' "$what"
  else
    printf 'FAIL %s\n' "$what" >&2
    failures=$((failures + 1))
  fi
}

DATA="$TMP_ROOT/userdata/portmaster"
CONTROL="$DATA/PortMaster"
ARMHF="$DATA/compat/armhf"
mkdir -p "$CONTROL" "$ARMHF/lib" "$ARMHF/bin" \
  "$ARMHF/usr/lib/arm-linux-gnueabihf/mali" "$TMP_ROOT/run"
: >"$CONTROL/control.txt"
: >"$ARMHF/lib/ld-linux-armhf.so.3"
printf '#!/bin/sh\nexec "$@"\n' >"$ARMHF/bin/leaf-armhf-run"
chmod 755 "$ARMHF/bin/leaf-armhf-run"
: >"$ARMHF/usr/lib/arm-linux-gnueabihf/mali/libEGL.so.1"
: >"$ARMHF/usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0"

# A real Unix socket stands in for Leaf's Weston.
HOST_SOCKET="$TMP_ROOT/run/wayland-0"
python3 -I -c '
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.bind(sys.argv[1])
s.listen(1)
time.sleep(120)
' "$HOST_SOCKET" &
SOCKET_PID=$!
for _ in $(seq 1 50); do
  [ -S "$HOST_SOCKET" ] && break
  sleep 0.1
done

write_hook() {
  SDCARD_PATH="$TMP_ROOT" \
  USERDATA_PATH="$TMP_ROOT/userdata" \
  PORTMASTER_MLP1_DATA_DIR="$DATA" \
  PORTMASTER_CONTROLFOLDER="$CONTROL" \
    "$ROOT/scripts/write-leaf-runtime-hook.sh"
}
write_hook

HOOK="$CONTROL/leaf-armhf-env.sh"
WRITER_SUM="$(cksum <"$ROOT/scripts/write-leaf-runtime-hook.sh" | cut -d' ' -f1)"
check "hook records its writer" grep -qx "# LEAF_PM_HOOK_WRITER=$WRITER_SUM" "$HOOK"
check "mod_Leaf.txt is installed" test -f "$CONTROL/mod_Leaf.txt"
check "leaf-gptokeyb-run is executable" test -x "$CONTROL/leaf-gptokeyb-run"
check "hook is valid sh" sh -n "$HOOK"
check "leaf-gptokeyb-run is valid sh" sh -n "$CONTROL/leaf-gptokeyb-run"
check "mod_Leaf.txt is valid bash" bash -n "$CONTROL/mod_Leaf.txt"

# Stands in for Leaf's armhf wrapper: sources the hook with sh, then reports.
WRAPPER_PROBE="$TMP_ROOT/wrapper-probe.sh"
cat >"$WRAPPER_PROBE" <<'EOF'
. "$controlfolder/leaf-armhf-env.sh" >/dev/null 2>&1
for v in HOME XDG_DATA_HOME LEAF_PM_HOST_WAYLAND_SOCKET CRUSTY_LIBEGL \
         CRUSTY_LIBSDL CRUSTY_WLMODE CRUSTY_WLDISP BOX86_LIBGL; do
  eval "printf '%s=%s\n' \"\$v\" \"\${$v-}\""
done
printf 'SDL_VIDEODRIVER=%s\n' "${SDL_VIDEODRIVER-<unset>}"
EOF

# Runs a port-like sequence in a clean environment and prints NAME=value lines.
# $1: commands between the first source (control.txt) and the second one (the
# armhf wrapper).
port_env() {
  env -i PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" \
    HOME="/root" XDG_RUNTIME_DIR="$TMP_ROOT/run" SDL_VIDEODRIVER=wayland \
    controlfolder="$CONTROL" SDCARD_PATH="$TMP_ROOT" PROBE="$WRAPPER_PROBE" \
    bash -c '
      . "$controlfolder/leaf-armhf-env.sh" >/dev/null 2>&1
      eval "$1"
      sh "$PROBE"
    ' port "$1"
}

expect() {
  local what="$1" out="$2" line="$3"
  check "$what" grep -qx -- "$line" <<<"$out"
}

out="$(port_env 'export XDG_DATA_HOME=/ports/game/conf HOME=/ports/game/home')"
expect "nested source keeps the port's XDG_DATA_HOME" "$out" "XDG_DATA_HOME=/ports/game/conf"
expect "nested source keeps the port's HOME" "$out" "HOME=/ports/game/home"
expect "first source records Leaf's Wayland socket" "$out" "LEAF_PM_HOST_WAYLAND_SOCKET=$HOST_SOCKET"

out="$(port_env ':')"
expect "first source points XDG_DATA_HOME at the data folder" "$out" "XDG_DATA_HOME=$DATA"
expect "first source points HOME at the data folder" "$out" "HOME=$DATA"
expect "plain armhf launch leaves SDL_VIDEODRIVER alone" "$out" "SDL_VIDEODRIVER=wayland"
expect "plain armhf launch leaves Crusty alone" "$out" "CRUSTY_WLMODE="
expect "plain armhf launch leaves box86's libGL alone" "$out" "BOX86_LIBGL="

WP32='export WP_32BIT=1 gllib_preload=/tmp/weston/lib_armhf/graphics/crusty_glx//libcrusty.so CRUSTY_LIBEGL= CRUSTY_LIBSDL= CRUSTY_WLMODE=0 CRUSTY_WLDISP=wayland-5'
out="$(port_env "$WP32")"
expect "westonpack32 sets Crusty's EGL" "$out" "CRUSTY_LIBEGL=$ARMHF/usr/lib/arm-linux-gnueabihf/mali/libEGL.so.1"
expect "westonpack32 sets Crusty's SDL" "$out" "CRUSTY_LIBSDL=$ARMHF/usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0"
expect "westonpack32 uses Crusty's Wayland mode" "$out" "CRUSTY_WLMODE=1"
expect "westonpack32 shows Crusty on Leaf's Weston" "$out" "CRUSTY_WLDISP=$HOST_SOCKET"
expect "westonpack32 clears Leaf's SDL_VIDEODRIVER" "$out" "SDL_VIDEODRIVER=<unset>"
expect "westonpack32 gives box86 gl4es" "$out" "BOX86_LIBGL=libGL.so.1"

out="$(port_env "$WP32 CRUSTY_LIBEGL=/custom/libEGL.so BOX86_LIBGL=custom.so")"
expect "westonpack32 keeps a found EGL" "$out" "CRUSTY_LIBEGL=/custom/libEGL.so"
expect "westonpack32 keeps a chosen box86 libGL" "$out" "BOX86_LIBGL=custom.so"

out="$(port_env "$WP32 SDL_VIDEODRIVER=x11")"
expect "westonpack32 keeps a port's own SDL_VIDEODRIVER" "$out" "SDL_VIDEODRIVER=x11"

out="$(port_env "$WP32 LEAF_PM_WESTONPACK32_COMPAT=0")"
expect "LEAF_PM_WESTONPACK32_COMPAT=0 turns it off" "$out" "CRUSTY_WLMODE=0"

out="$(port_env 'export WP_32BIT=1 gllib_preload=/tmp/weston/lib_armhf/graphics/gl4es_x11/libGL.so.1')"
expect "westonpack32 without Crusty is left alone" "$out" "CRUSTY_WLMODE="

# mod_Leaf.txt wraps both gptokeyb commands once, even when sourced twice.
mod_out="$(controlfolder="$CONTROL" bash -c '
  GPTOKEYB2=" env LD_PRELOAD=/pm/libinterpose.aarch64.so /pm/gptokeyb2 -sudokill"
  GPTOKEYB=" /pm/gptokeyb -sudokill"
  . "$controlfolder/mod_Leaf.txt"; . "$controlfolder/mod_Leaf.txt"
  printf "%s\n%s\n" "$GPTOKEYB2" "$GPTOKEYB"')"
check "mod_Leaf.txt wraps GPTOKEYB2 once" grep -qx -- "$CONTROL/leaf-gptokeyb-run  env LD_PRELOAD=/pm/libinterpose.aarch64.so /pm/gptokeyb2 -sudokill" <<<"$mod_out"
check "mod_Leaf.txt wraps GPTOKEYB once" grep -qx -- "$CONTROL/leaf-gptokeyb-run  /pm/gptokeyb -sudokill" <<<"$mod_out"
mod_out="$(controlfolder="$CONTROL" bash -c 'unset GPTOKEYB GPTOKEYB2; . "$controlfolder/mod_Leaf.txt"; printf "[%s][%s]\n" "${GPTOKEYB2-}" "${GPTOKEYB-}"')"
check "mod_Leaf.txt leaves unset gptokeyb commands unset" test "$mod_out" = "[][]"

# leaf-gptokeyb-run execs the command it wraps: same output, same exit code.
run_out="$(LEAF_PM_GPTOKEYB_EXPOSE_TICKS=1 "$CONTROL/leaf-gptokeyb-run" printf '%s' wrapped)"
check "leaf-gptokeyb-run runs its command" test "$run_out" = wrapped
set +e
LEAF_PM_GPTOKEYB_EXPOSE_TICKS=1 "$CONTROL/leaf-gptokeyb-run" sh -c 'exit 3'
rc=$?
set -e
check "leaf-gptokeyb-run keeps the exit code" test "$rc" = 3
pid_out="$(LEAF_PM_GPTOKEYB_EXPOSE_TICKS=1 "$CONTROL/leaf-gptokeyb-run" sh -c 'echo $$' & wait_pid=$!; wait "$wait_pid" >/dev/null; echo "$wait_pid")"
check "leaf-gptokeyb-run execs in place" test "$(sed -n 1p <<<"$pid_out")" = "$(sed -n 2p <<<"$pid_out")"

# bind_directories/bind_files also bind the other spelling of a target under
# ~/.local/share or $XDG_DATA_HOME, which Leaf splits. Mounts need a device;
# these check the mapping and the save adoption.
alias_of() {
  env -i PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" HOME="$DATA" XDG_DATA_HOME="$DATA" \
    controlfolder="$CONTROL" SDCARD_PATH="$TMP_ROOT" ALIAS_ENV="${2:-}" \
    sh -c '. "$controlfolder/leaf-armhf-env.sh" >/dev/null 2>&1; [ -n "$ALIAS_ENV" ] && eval "$ALIAS_ENV"; leaf_pm_bind_alias_target "$1" || echo "<none>"' alias "$1"
}
check "alias: ~/.local/share/<name> also binds \$XDG_DATA_HOME/<name>" \
  test "$(alias_of "$DATA/.local/share/perfectdark")" = "$DATA/perfectdark"
check "alias: nested path keeps its tail" \
  test "$(alias_of "$DATA/.local/share/Owlboy/Saves")" = "$DATA/Owlboy/Saves"
check "alias: \$XDG_DATA_HOME/<name> also binds ~/.local/share/<name>" \
  test "$(alias_of "$DATA/Outrage")" = "$DATA/.local/share/Outrage"
check "alias: ~/.config is left alone" test "$(alias_of "$DATA/.config/Owlboy")" = "<none>"
check "alias: other dot folders are left alone" test "$(alias_of "$DATA/.openmohaa")" = "<none>"
check "alias: never onto Leaf's PortMaster folder" test "$(alias_of "$DATA/.local/share/PortMaster")" = "<none>"
check "alias: never onto Leaf's compat folder" test "$(alias_of "$DATA/.local/share/compat/x")" = "<none>"
check "alias: paths outside both bases are left alone" test "$(alias_of "/mnt/other/game")" = "<none>"
check "alias: none when the two bases match" \
  test "$(alias_of "$DATA/.local/share/x" "XDG_DATA_HOME=\$HOME/.local/share")" = "<none>"
check "alias: LEAF_PM_BIND_XDG_ALIAS=0 turns it off" \
  test "$(alias_of "$DATA/.local/share/x" "LEAF_PM_BIND_XDG_ALIAS=0")" = "<none>"

ADOPT="$TMP_ROOT/adopt"
mkdir -p "$ADOPT/old/Saves/slot" "$ADOPT/port/Saves"
printf old >"$ADOPT/old/settings.cfg"
printf old >"$ADOPT/old/Saves/slot/1.sav"
printf old >"$ADOPT/old/Saves/shared.sav"
printf old >"$ADOPT/old/.hidden"
printf port >"$ADOPT/port/Saves/shared.sav"
env -i PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" HOME="$DATA" XDG_DATA_HOME="$DATA" \
  controlfolder="$CONTROL" SDCARD_PATH="$TMP_ROOT" ADOPT="$ADOPT" \
  sh -c '. "$controlfolder/leaf-armhf-env.sh" >/dev/null 2>&1; leaf_pm_bind_adopt "$ADOPT/old" "$ADOPT/port"'
check "adopt: moves a file the port folder lacks" test "$(cat "$ADOPT/port/settings.cfg" 2>/dev/null)" = old
check "adopt: moves dot files too" test -f "$ADOPT/port/.hidden"
check "adopt: merges into an existing folder" test "$(cat "$ADOPT/port/Saves/slot/1.sav" 2>/dev/null)" = old
check "adopt: never replaces the port's file" test "$(cat "$ADOPT/port/Saves/shared.sav")" = port
check "adopt: leaves the conflicting file where it was" test "$(cat "$ADOPT/old/Saves/shared.sav")" = old
check "adopt: moved files are gone from the old place" test ! -e "$ADOPT/old/settings.cfg"
env -i PATH="$PATH" HOME="$DATA" XDG_DATA_HOME="$DATA" controlfolder="$CONTROL" SDCARD_PATH="$TMP_ROOT" ADOPT="$ADOPT" \
  sh -c '. "$controlfolder/leaf-armhf-env.sh" >/dev/null 2>&1; leaf_pm_bind_adopt "$ADOPT/port" "$ADOPT/port"'
check "adopt: a folder onto itself is a no-op" test "$(cat "$ADOPT/port/Saves/shared.sav")" = port

# Init services (Weston, Leaf) stop and start with a boot-like environment,
# never the port's LD_PRELOAD, LD_LIBRARY_PATH or rotate setting. The stubs log
# what they were started with.
INIT="$TMP_ROOT/init.d"
INIT_LOG="$TMP_ROOT/init.log"
UPSTREAM_LOG="$TMP_ROOT/upstream.log"
mkdir -p "$INIT"
for svc in S49weston S50leaf; do
  cat >"$INIT/$svc" <<EOF
#!/bin/sh
printf '%s %s preload=[%s] libpath=[%s] rotate=[%s]\n' "$svc" "\$1" "\${LD_PRELOAD:-}" "\${LD_LIBRARY_PATH:-}" "\${LEAF_DRM_ROTATE:-}" >>"$INIT_LOG"
EOF
  chmod 755 "$INIT/$svc"
done

# A fake g29 Mali, rotate shim, an SDL3 with bmdhacks' sdl2 backend and one
# without, and an HDMI connector.
MALI64="$DATA/compat/mali/aarch64"
ROTATE="$DATA/compat/drm/aarch64/leaf-drm-rotate.so"
SDL3GAME="$TMP_ROOT/ports/sdl3game"
PLAINGAME="$TMP_ROOT/ports/plainsdl3"
DRM="$TMP_ROOT/drm"
mkdir -p "$MALI64" "$(dirname "$ROTATE")" "$SDL3GAME/libs.aarch64" \
  "$PLAINGAME/libs.aarch64" "$DRM/card0-HDMI-A-1"
: >"$MALI64/libmali.so.1"
: >"$ROTATE"
printf 'ELF ... SDL3SHIM_SDL2_VIDEODRIVER ...' >"$SDL3GAME/libs.aarch64/libSDL3.so.0"
printf 'ELF ... plain SDL3 ...' >"$PLAINGAME/libs.aarch64/libSDL3.so.0"
echo disconnected >"$DRM/card0-HDMI-A-1/status"

# Runs a port's launch: control.txt and mod_Leaf.txt, the port's own setup
# ($2), pm_platform_helper on the game binary ($1; "sub:" runs it in a
# subshell), then prints what the game would see. The EXIT trap still fires.
launch_env() {
  : >"$INIT_LOG"
  : >"$UPSTREAM_LOG"
  env -i PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" HOME=/root \
    controlfolder="$CONTROL" SDCARD_PATH="$TMP_ROOT" UPSTREAM_LOG="$UPSTREAM_LOG" \
    LEAF_PM_INIT_DIR="$INIT" LEAF_PM_DRM_SYSFS="$DRM" \
    LEAF_PM_JAVA_WESTON_STOP_DELAY=0 LEAF_PM_JAVA_WESTON_START_DELAY=0 \
    LEAF_PM_MALI_AARCH64_DIR="$MALI64" LEAF_PM_AARCH64_DRM_ROTATE_SHIM="$ROTATE" \
    bash -c '
      pidof() { return 0; }
      pm_platform_helper() { echo "upstream $1" >>"$UPSTREAM_LOG"; }
      . "$controlfolder/leaf-armhf-env.sh" >/dev/null 2>&1
      . "$controlfolder/mod_Leaf.txt"
      eval "$2"
      case "$1" in
        sub:*) ( pm_platform_helper "${1#sub:}" ) 2>/dev/null ;;
        *) pm_platform_helper "$1" 2>/dev/null ;;
      esac
      printf "LIBPATH=%s\nPRELOAD=%s\nSHIMVIDEO=%s\nROTATE=%s\n" \
        "${LD_LIBRARY_PATH:-}" "${LD_PRELOAD:-}" \
        "${SDL3SHIM_SDL2_VIDEODRIVER:-<unset>}" "${LEAF_DRM_ROTATE:-<unset>}"
    ' port "$1" "$2"
}
SDL3_SETUP="export LD_LIBRARY_PATH=$SDL3GAME/libs.aarch64:\${LD_LIBRARY_PATH:-} SDL_VIDEODRIVER=sdl2"
CLEAN_ENV="preload=[] libpath=[] rotate=[]"

out="$(launch_env "$SDL3GAME/game.aarch64" "$SDL3_SETUP")"
expect "sdl3-on-sdl2: g29 Mali first on the library path" "$out" "LIBPATH=$MALI64:$SDL3GAME/libs.aarch64:"
check "sdl3-on-sdl2: rotate shim preloaded first" grep -q "^PRELOAD=$ROTATE" <<<"$out"
expect "sdl3-on-sdl2: SDL2 draws through kmsdrm" "$out" "SHIMVIDEO=kmsdrm"
expect "sdl3-on-sdl2: panel rotated landscape" "$out" "ROTATE=270"
check "sdl3-on-sdl2: Weston stopped with a clean environment" grep -qxF "S49weston stop $CLEAN_ENV" "$INIT_LOG"
check "sdl3-on-sdl2: Weston restarted on exit with a clean environment" grep -qxF "S49weston start $CLEAN_ENV" "$INIT_LOG"
check "sdl3-on-sdl2: PortMaster's own pm_platform_helper still runs" grep -qx "upstream $SDL3GAME/game.aarch64" "$UPSTREAM_LOG"

out="$(launch_env "$SDL3GAME/game.aarch64" "$SDL3_SETUP LEAF_PM_SDL3_ON_SDL2_DIRECT_DRM=0")"
expect "sdl3-on-sdl2: LEAF_PM_SDL3_ON_SDL2_DIRECT_DRM=0 turns it off" "$out" "SHIMVIDEO=<unset>"
check "sdl3-on-sdl2: switched off leaves Weston running" test ! -s "$INIT_LOG"

out="$(launch_env "$PLAINGAME/game.aarch64" "export LD_LIBRARY_PATH=$PLAINGAME/libs.aarch64 SDL_VIDEODRIVER=sdl2")"
expect "sdl3-on-sdl2: an SDL3 without the sdl2 backend is left alone" "$out" "SHIMVIDEO=<unset>"

out="$(launch_env "$SDL3GAME/game.aarch64" "export LD_LIBRARY_PATH=$SDL3GAME/libs.aarch64 SDL_VIDEODRIVER=wayland")"
expect "sdl3-on-sdl2: a port that did not pick sdl2 is left alone" "$out" "SHIMVIDEO=<unset>"

echo connected >"$DRM/card0-HDMI-A-1/status"
out="$(launch_env "$SDL3GAME/game.aarch64" "$SDL3_SETUP")"
expect "sdl3-on-sdl2: stays on Weston while HDMI is connected" "$out" "SHIMVIDEO=<unset>"
check "sdl3-on-sdl2: HDMI leaves Weston running" test ! -s "$INIT_LOG"
echo disconnected >"$DRM/card0-HDMI-A-1/status"

out="$(launch_env "sub:$SDL3GAME/game.aarch64" "$SDL3_SETUP")"
check "sdl3-on-sdl2: never from a subshell" test ! -s "$INIT_LOG"
check "sdl3-on-sdl2: a subshell call still runs PortMaster's body" grep -qx "upstream $SDL3GAME/game.aarch64" "$UPSTREAM_LOG"

wrap_out="$(env -i PATH="$PATH" controlfolder="$CONTROL" bash -c '
  pm_platform_helper() { :; }
  . "$controlfolder/mod_Leaf.txt"; . "$controlfolder/mod_Leaf.txt"
  declare -f pm_platform_helper | grep -c leaf_pm_before_game_start
  pm_platform_helper() { :; }
  . "$controlfolder/mod_Leaf.txt"
  declare -f pm_platform_helper | grep -c leaf_pm_platform_helper_upstream')"
check "mod_Leaf.txt wraps pm_platform_helper once" test "$(sed -n 1p <<<"$wrap_out")" = 2
check "mod_Leaf.txt rewraps a redefined pm_platform_helper" test "$(sed -n 2p <<<"$wrap_out")" = 1

: >"$INIT_LOG"
env -i PATH="$PATH" HOME=/root controlfolder="$CONTROL" SDCARD_PATH="$TMP_ROOT" LEAF_PM_INIT_DIR="$INIT" \
  LD_PRELOAD="$ROTATE" LD_LIBRARY_PATH="$MALI64" LEAF_DRM_ROTATE=270 \
  bash -c '. "$controlfolder/leaf-armhf-env.sh" >/dev/null 2>&1
    sleep() { :; }
    export LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE_STACK_STOPPED=1
    export LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE_WESTON_WAS_RUNNING=1
    export LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE_LEAF_WAS_RUNNING=1
    leaf_pm_restore_gothic_machismo_vulkan_rotate'
check "gothic restore starts Weston with a clean environment" grep -qxF "S49weston start $CLEAN_ENV" "$INIT_LOG"
check "gothic restore starts Leaf with a clean environment" grep -qxF "S50leaf start $CLEAN_ENV" "$INIT_LOG"

# prepare-port-runtime.sh rewrites a hook from another writer, then leaves it.
prepare() {
  SDCARD_PATH="$TMP_ROOT" USERDATA_PATH="$TMP_ROOT/userdata" \
  PORTMASTER_MLP1_DATA_DIR="$DATA" PORTMASTER_CONTROLFOLDER="$CONTROL" \
  LEAF_PM_PATCH_ON_LAUNCH=0 LEAF_PM_LOOP_NODES=0 LEAF_PM_ZRAM=0 \
    sh "$ROOT/scripts/prepare-port-runtime.sh"
}
sed -i.bak "s/^# LEAF_PM_HOOK_WRITER=.*/# LEAF_PM_HOOK_WRITER=0/" "$HOOK" && rm -f "$HOOK.bak"
rm -f "$CONTROL/mod_Leaf.txt"
prep_out="$(prepare)"
check "prepare refreshes a stale hook" grep -q "refreshed the Leaf runtime hook" <<<"$prep_out"
check "refreshed hook records the current writer" grep -qx "# LEAF_PM_HOOK_WRITER=$WRITER_SUM" "$HOOK"
check "refresh restores mod_Leaf.txt" test -f "$CONTROL/mod_Leaf.txt"
prep_out="$(prepare)"
check "prepare leaves a current hook alone" test -z "$(grep "runtime hook" <<<"$prep_out" || true)"

if [ "$failures" -ne 0 ]; then
  printf 'runtime hook fixtures: %d failure(s)\n' "$failures" >&2
  exit 1
fi
printf 'runtime hook fixtures passed\n'
