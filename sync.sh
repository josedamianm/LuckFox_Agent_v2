#!/bin/bash

BOARD_LOCAL="root@192.168.1.60"
BOARD_REMOTE="root@luckfoxpico1.aiserver.onmobilespace.com"
BOARD_REMOTE_PORT="8022"

BOARD="$BOARD_REMOTE"
SSH_OPTS="-p $BOARD_REMOTE_PORT"
SCP_OPTS="-P $BOARD_REMOTE_PORT"
RSYNC_FLAGS="-av --no-perms --no-owner --no-group"

if [ "${1:-}" = "--private" ]; then
    BOARD="$BOARD_LOCAL"
    SSH_OPTS=""
    SCP_OPTS=""
    shift
fi

FILE_MAP=(
    "lvgl_gui/build/luckfox_gui:/root/Executables/luckfox_gui"
    "board/root/main.py:/root/main.py"
    "board/root/enable_spi0_spidev.dtbo:/root/enable_spi0_spidev.dtbo"
    "board/root/enable_spi0_spidev.dts:/root/enable_spi0_spidev.dts"
    "board/root/enable_uart2.dts:/root/enable_uart2.dts"
    "board/sdcard/gui_client.py:/mnt/sdcard/gui_client.py"
    "board/sdcard/http_api_server_v2.py:/mnt/sdcard/http_api_server_v2.py"
    "board/sdcard/http_api_server.py:/mnt/sdcard/http_api_server.py"
    "board/sdcard/rtsp_capture.py:/mnt/sdcard/rtsp_capture.py"
    "board/sdcard/frpc.toml:/mnt/sdcard/frpc.toml"
    "board/sdcard/audio_sender.py:/mnt/sdcard/audio_sender.py"
    "board/init.d/S20spi0overlay:/etc/init.d/S20spi0overlay"
    "board/init.d/S21uart2overlay:/etc/init.d/S21uart2overlay"
    "board/init.d/S50rtcinit:/etc/init.d/S50rtcinit"
    "board/init.d/S60ntpd:/etc/init.d/S60ntpd"
    "board/init.d/S99button_pullups:/etc/init.d/S99button_pullups"
    "board/init.d/S99python:/etc/init.d/S99python"
    "board/init.d/S98frpc:/etc/init.d/S98frpc"
    "board/executables/get_frame.c:/root/Executables/get_frame.c"
)

get_remote() {
    local target="$1"
    for entry in "${FILE_MAP[@]}"; do
        local local_path="${entry%%:*}"
        local remote_path="${entry#*:}"
        if [ "$local_path" = "$target" ]; then
            echo "$remote_path"
            return
        fi
    done
}

usage() {
    echo "Usage: $0 [--private] <push|pull|diff|status> [local_path]"
    echo ""
    echo "  --private           Use local network IP (192.168.1.60) instead of public domain"
    echo "  push              Push all tracked files to the board"
    echo "  push <path>       Push a specific file (e.g. board/sdcard/http_api_server.py)"
    echo "  pull              Pull all tracked files from the board"
    echo "  pull <path>       Pull a specific file (e.g. board/sdcard/http_api_server.py)"
    echo "  diff              Show diff for all files that differ from the board"
    echo "  diff <path>       Show diff for a specific file"
    echo "  status            Show which files differ between local and board"
    exit 1
}

do_push() {
    local local_path="$1"
    local remote_path
    remote_path=$(get_remote "$local_path")
    if [ -z "$remote_path" ]; then
        echo "ERROR: '$local_path' is not a tracked file"
        exit 1
    fi
    echo "→ pushing $local_path to $BOARD:$remote_path"
    if [ -n "$SSH_OPTS" ]; then
        rsync $RSYNC_FLAGS -e "ssh $SSH_OPTS" "$local_path" "$BOARD:$remote_path"
    else
        rsync $RSYNC_FLAGS "$local_path" "$BOARD:$remote_path"
    fi
}

do_pull() {
    local local_path="$1"
    local remote_path
    remote_path=$(get_remote "$local_path")
    if [ -z "$remote_path" ]; then
        echo "ERROR: '$local_path' is not a tracked file"
        exit 1
    fi
    echo "← pulling $BOARD:$remote_path to $local_path"
    if [ -n "$SSH_OPTS" ]; then
        rsync $RSYNC_FLAGS -e "ssh $SSH_OPTS" "$BOARD:$remote_path" "$local_path"
    else
        rsync $RSYNC_FLAGS "$BOARD:$remote_path" "$local_path"
    fi
}

do_diff() {
    local local_path="$1"
    local remote_path
    remote_path=$(get_remote "$local_path")
    if [ -z "$remote_path" ]; then
        echo "ERROR: '$local_path' is not a tracked file"
        exit 1
    fi
    local tmp
    tmp=$(mktemp)
    if [ -n "$SCP_OPTS" ]; then
        scp -q $SCP_OPTS "$BOARD:$remote_path" "$tmp" 2>/dev/null
    else
        scp -q "$BOARD:$remote_path" "$tmp" 2>/dev/null
    fi
    diff --color=always "$local_path" "$tmp" && echo "(no diff)"
    rm -f "$tmp"
}

CMD="${1:-}"
TARGET="${2:-}"

case "$CMD" in
    push)
        if [ -n "$TARGET" ]; then
            do_push "$TARGET"
        else
            echo "=== Pushing all files to board ==="
            for entry in "${FILE_MAP[@]}"; do
                do_push "${entry%%:*}"
            done
            echo "=== Push complete ==="
        fi
        ;;
    pull)
        if [ -n "$TARGET" ]; then
            do_pull "$TARGET"
        else
            echo "=== Pulling all files from board ==="
            for entry in "${FILE_MAP[@]}"; do
                do_pull "${entry%%:*}"
            done
            echo "=== Pull complete ==="
        fi
        ;;
    diff)
        if [ -n "$TARGET" ]; then
            do_diff "$TARGET"
        else
            echo "=== Diffing all tracked files ==="
            for entry in "${FILE_MAP[@]}"; do
                local_path="${entry%%:*}"
                remote_path="${entry#*:}"
                tmp=$(mktemp)
                scp -q $SCP_OPTS "$BOARD:$remote_path" "$tmp" 2>/dev/null
                if ! diff -q "$local_path" "$tmp" > /dev/null 2>&1; then
                    echo ""
                    echo "--- DIFF: $local_path vs board:$remote_path ---"
                    diff --color=always "$local_path" "$tmp"
                fi
                rm -f "$tmp"
            done
            echo "=== Diff complete ==="
        fi
        ;;
    status)
        echo "=== Checking status of all tracked files ==="
        all_match=true
        for entry in "${FILE_MAP[@]}"; do
            local_path="${entry%%:*}"
            remote_path="${entry#*:}"
            tmp=$(mktemp)
            scp -q $SCP_OPTS "$BOARD:$remote_path" "$tmp" 2>/dev/null
            if diff -q "$local_path" "$tmp" > /dev/null 2>&1; then
                echo "  ✓  $local_path"
            else
                echo "  ✗  $local_path  (differs from board:$remote_path)"
                all_match=false
            fi
            rm -f "$tmp"
        done
        $all_match && echo "" && echo "All files in sync."
        ;;
    *)
        usage
        ;;
esac
