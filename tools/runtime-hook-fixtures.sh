#!/usr/bin/env bash
# Host checks for the runtime hook's launch-time behavior: the files the writer
# installs next to it, the first-source-only HOME/XDG_DATA_HOME rule, the
# 32-bit Westonpack adjustments, mod_Leaf.txt, leaf-gptokeyb-run, and the
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
