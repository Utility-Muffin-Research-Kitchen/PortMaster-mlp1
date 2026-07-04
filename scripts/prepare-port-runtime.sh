#!/bin/sh
# Prepare PortMaster-only runtime features immediately before a port launches.
#
# Everything here is intentionally reboot-clean:
#   - /dev is devtmpfs, so extra loop nodes vanish at reboot.
#   - zram swap state lives in the running kernel and is not persisted.
# The script must not write stock OS/eMMC configuration such as /etc/fstab.

PREFIX="${LEAF_PM_PORT_RUNTIME_LOG_PREFIX:-portmaster-runtime:}"
LOOP_ENABLED="${LEAF_PM_LOOP_NODES:-1}"
LOOP_COUNT="${LEAF_PM_LOOP_COUNT:-16}"
ZRAM_ENABLED="${LEAF_PM_ZRAM:-1}"
ZRAM_DEV="${LEAF_PM_ZRAM_DEV:-/dev/zram0}"
ZRAM_SYS="/sys/class/block/${ZRAM_DEV##*/}"
ZRAM_SIZE_BYTES="${LEAF_PM_ZRAM_SIZE_BYTES:-268435456}"
ZRAM_PRIORITY="${LEAF_PM_ZRAM_PRIORITY:-100}"
ZRAM_COMP_ALGORITHM="${LEAF_PM_ZRAM_COMP_ALGORITHM:-}"

log() {
    echo "$PREFIX $*"
}

is_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

ensure_loop_nodes() {
    [ "$LOOP_ENABLED" = "0" ] && return 0

    if ! is_uint "$LOOP_COUNT"; then
        log "invalid LEAF_PM_LOOP_COUNT=$LOOP_COUNT; using 16"
        LOOP_COUNT=16
    fi
    if [ "$LOOP_COUNT" -lt 8 ] 2>/dev/null; then
        LOOP_COUNT=8
    fi
    if [ "$LOOP_COUNT" -gt 64 ] 2>/dev/null; then
        log "LEAF_PM_LOOP_COUNT=$LOOP_COUNT is high; capping at 64"
        LOOP_COUNT=64
    fi

    if [ ! -e /dev/loop-control ]; then
        if mknod /dev/loop-control c 10 237 2>/dev/null; then
            chgrp disk /dev/loop-control 2>/dev/null || true
            chmod 660 /dev/loop-control 2>/dev/null || true
            log "created /dev/loop-control"
        else
            log "could not create /dev/loop-control"
        fi
    fi

    created=0
    i=0
    while [ "$i" -lt "$LOOP_COUNT" ]; do
        node="/dev/loop$i"
        if [ -e "$node" ]; then
            if [ ! -b "$node" ]; then
                log "$node exists but is not a block device"
            fi
            i=$((i + 1))
            continue
        fi

        if mknod "$node" b 7 "$i" 2>/dev/null; then
            chgrp disk "$node" 2>/dev/null || true
            chmod 660 "$node" 2>/dev/null || true
            created=$((created + 1))
        else
            log "could not create $node"
        fi
        i=$((i + 1))
    done

    [ "$created" -gt 0 ] && log "created $created loop device node(s)"
}

zram_is_active() {
    awk -v dev="$ZRAM_DEV" '$1 == dev { found = 1; exit } END { exit found ? 0 : 1 }' /proc/swaps 2>/dev/null
}

run_mkswap() {
    if command -v mkswap >/dev/null 2>&1; then
        mkswap "$ZRAM_DEV" >/dev/null 2>&1
        return $?
    fi
    if command -v busybox >/dev/null 2>&1; then
        busybox mkswap "$ZRAM_DEV" >/dev/null 2>&1
        return $?
    fi
    return 127
}

run_swapon() {
    if command -v swapon >/dev/null 2>&1; then
        swapon -p "$ZRAM_PRIORITY" "$ZRAM_DEV" >/dev/null 2>&1 &&
            return 0
        swapon "$ZRAM_DEV" >/dev/null 2>&1
        return $?
    fi
    if command -v busybox >/dev/null 2>&1; then
        busybox swapon -p "$ZRAM_PRIORITY" "$ZRAM_DEV" >/dev/null 2>&1 &&
            return 0
        busybox swapon "$ZRAM_DEV" >/dev/null 2>&1
        return $?
    fi
    return 127
}

ensure_zram_swap() {
    [ "$ZRAM_ENABLED" = "0" ] && return 0
    [ -b "$ZRAM_DEV" ] || {
        log "$ZRAM_DEV missing; skipping zram swap"
        return 0
    }
    [ -d "$ZRAM_SYS" ] || {
        log "$ZRAM_SYS missing; skipping zram swap"
        return 0
    }
    zram_is_active && return 0

    if ! is_uint "$ZRAM_SIZE_BYTES"; then
        log "invalid LEAF_PM_ZRAM_SIZE_BYTES=$ZRAM_SIZE_BYTES; using 268435456"
        ZRAM_SIZE_BYTES=268435456
    fi

    if [ -n "$ZRAM_COMP_ALGORITHM" ] && [ -w "$ZRAM_SYS/comp_algorithm" ]; then
        echo "$ZRAM_COMP_ALGORITHM" >"$ZRAM_SYS/comp_algorithm" 2>/dev/null ||
            log "could not set zram compressor to $ZRAM_COMP_ALGORITHM"
    fi

    current_size="$(cat "$ZRAM_SYS/disksize" 2>/dev/null || echo 0)"
    case "$current_size" in ''|*[!0-9]*) current_size=0 ;; esac

    if [ "$current_size" -eq 0 ] 2>/dev/null; then
        if ! echo "$ZRAM_SIZE_BYTES" >"$ZRAM_SYS/disksize" 2>/dev/null; then
            log "could not set $ZRAM_DEV disksize to $ZRAM_SIZE_BYTES"
            return 0
        fi
    fi

    if ! run_mkswap; then
        zram_is_active && return 0
        log "mkswap failed for $ZRAM_DEV"
        return 0
    fi

    if run_swapon; then
        log "enabled $ZRAM_DEV swap size=$ZRAM_SIZE_BYTES priority=$ZRAM_PRIORITY"
    else
        zram_is_active && return 0
        log "swapon failed for $ZRAM_DEV"
    fi
}

ensure_loop_nodes
ensure_zram_swap
exit 0
