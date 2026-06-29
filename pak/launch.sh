#!/bin/sh
set -eu

PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

find_sdcard_root() {
  probe="$PAK_DIR"
  while [ "$probe" != "/" ] && [ -n "$probe" ]; do
    if [ -d "$probe/.system/leaf/platforms" ] ||
       { [ -d "$probe/Apps" ] && [ -d "$probe/.system" ]; }; then
      printf '%s\n' "$probe"
      return 0
    fi
    probe="$(dirname "$probe")"
  done

  case "$PAK_DIR" in
    */Apps/*/*.pak) (CDPATH= cd -- "$PAK_DIR/../../.." && pwd) ;;
    *) printf '%s\n' "${SDCARD_PATH:-/mnt/sdcard}" ;;
  esac
}

PLATFORM="${PLATFORM:-mlp1}"
PAK_SDCARD_ROOT="$(find_sdcard_root)"
export SDCARD_PATH="${SDCARD_PATH:-${JAWAKA_SDCARD_ROOT:-$PAK_SDCARD_ROOT}}"

env_sh="$SDCARD_PATH/.system/leaf/platforms/$PLATFORM/launcher/env.sh"
if [ -n "${UMRK_ENV_FILE:-}" ] && [ -f "$UMRK_ENV_FILE" ]; then
  . "$UMRK_ENV_FILE"
elif [ -f "$env_sh" ]; then
  . "$env_sh"
fi

export PLATFORM="${PLATFORM:-mlp1}"
export PORTMASTER_MLP1_PAK_DIR="$PAK_DIR"
export PORTMASTER_MLP1_DATA_DIR="${PORTMASTER_MLP1_DATA_DIR:-${USERDATA_PATH:-$SDCARD_PATH/.userdata/$PLATFORM}/portmaster}"

LOG_DIR="${LOGS_PATH:-${USERDATA_PATH:-$SDCARD_PATH/.userdata/$PLATFORM}/logs}"
mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR=/tmp

if [ -n "${UMRK_LAUNCHER_PATH:-}" ] && [ -d "$UMRK_LAUNCHER_PATH/lib" ]; then
  export LD_LIBRARY_PATH="$UMRK_LAUNCHER_PATH/lib:${LD_LIBRARY_PATH:-}"
fi

cd "$PAK_DIR"
exec "$PAK_DIR/bin/portmaster-mlp1" "$@" 2>"$LOG_DIR/portmaster-mlp1.log"
