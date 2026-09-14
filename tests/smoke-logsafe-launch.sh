#!/usr/bin/env bash
# The Leaf 0.11 field failure: a session log that cannot take a byte killed
# this wrapper under set -e between "loading" and the port (the reporter fixed
# it by deleting the log by hand). The wrapper must reach the port in both
# unwritable-stdio cases:
#
#   closed fds        EBADF on every inherited write
#   read-only fds     a dup of a read-only fd as stdout and stderr
#
# RLIMIT_FSIZE=0 is deliberately not used as a whole-wrapper case: it rejects
# every file write including the config seeding the wrapper legitimately
# needs, which is stricter than the field condition. The SIGXFSZ/EFBIG class
# is covered at the probe level inside the preamble (ignored in a subshell) so
# the 4 GiB-ceiling case fails the probe instead of killing the shell.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WRAPPER="$ROOT_DIR/pak/leaf-platforms/mlp1/emulators/ports/launch.sh"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMP_ROOT"' EXIT

SDCARD="$TMP_ROOT/sd"
mkdir -p "$SDCARD/Roms/PORTS" "$SDCARD/bin"
PORT="$SDCARD/Roms/PORTS/Logsafe Test.sh"
STUB="$SDCARD/bin/retroarch"
EVIDENCE="$TMP_ROOT/evidence"

cat >"$PORT" <<EOF
#!/bin/sh
echo "port ran" >>"$EVIDENCE"
echo "port stdout"
EOF
chmod 0755 "$PORT"

cat >"$STUB" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 0755 "$STUB"

run_env() {
    printf '%s\n' \
        "LOGS_PATH=$TMP_ROOT/logs" \
        "UMRK_RETROARCH_BIN=$STUB" \
        "SDCARD_PATH=$SDCARD" \
        "USERDATA_PATH=$SDCARD/.userdata/mlp1" \
        "PLATFORM=mlp1"
}

# Closed fds: the preamble must fall back to /dev/null and the port must run.
rm -f "$EVIDENCE"
run_env xargs env >"$TMP_ROOT/env" || true
env $(sed 's/\n/ /g' "$TMP_ROOT/env") sh "$WRAPPER" "$PORT" >&- 2>&-
[ "$(cat "$EVIDENCE" 2>/dev/null)" = "port ran" ] ||
    { echo "closed-fd case: port did not run" >&2; exit 1; }
echo "closed-fd case: ok"

# Read-only fds: a real write() failure class (EACCES/EBADF on the dup), the
# closest portable stand-in for EIO without corrupting a filesystem.
rm -f "$EVIDENCE"
: >"$TMP_ROOT/readonly"
chmod 444 "$TMP_ROOT/readonly"
env $(sed 's/\n/ /g' "$TMP_ROOT/env") sh "$WRAPPER" "$PORT" \
    1<"$TMP_ROOT/readonly" 2>&1
[ "$(cat "$EVIDENCE" 2>/dev/null)" = "port ran" ] ||
    { echo "read-only-fd case: port did not run" >&2; exit 1; }
echo "read-only-fd case: ok"

# Healthy path: wrapper chatter lands in the session log, port output in the
# per-port log.
rm -f "$EVIDENCE" "$TMP_ROOT/session.log"
rm -rf "$TMP_ROOT/logs"
env $(sed 's/\n/ /g' "$TMP_ROOT/env") sh "$WRAPPER" "$PORT" \
    >>"$TMP_ROOT/session.log" 2>&1
grep -q "calibrated Leaf virtual gamepad not found" "$TMP_ROOT/session.log" ||
    { echo "healthy case: wrapper chatter missing from the session log" >&2
      cat "$TMP_ROOT/session.log" >&2; exit 1; }
grep -q "port stdout" "$TMP_ROOT/logs/ports/Logsafe Test.log" ||
    { echo "healthy case: port output missing from the per-port log" >&2; exit 1; }
echo "healthy case: ok"
