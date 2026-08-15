#!/usr/bin/env bash
# Capture raw events from the filtered keyboard (background mode).
#   sudo ./capture.sh start <name>   start and return to the prompt
#   sudo ./capture.sh stop           stop and summarise
#   sudo ./capture.sh status         current state
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIDF="$DIR/capture.pid"
CURF="$DIR/capture.current"
CFG=/etc/k70debounce/config

DEV=""
if [ -f "$CFG" ]; then
    . "$CFG"
    DEV="${DEVICE:-}"
fi
if [ -z "$DEV" ]; then
    for l in /dev/input/by-id/*-event-kbd /dev/input/by-path/*-event-kbd; do
        [ -e "$l" ] && DEV="$l" && break
    done
fi

cmd="${1:-}"

case "$cmd" in
start)
    NAME="${2:-session}"
    OUT="$DIR/${NAME}.log"

    if [ -z "$DEV" ]; then
        echo "ERROR: no device to capture (install the filter first, or" >&2
        echo "       edit DEVICE in $CFG)." >&2
        exit 1
    fi
    if [ ! -e "$DEV" ]; then
        echo "ERROR: device not found: $DEV" >&2
        exit 1
    fi
    if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
        echo "ERROR: a capture is already running (pid $(cat "$PIDF"))." >&2
        echo "Stop it with:  sudo $0 stop" >&2
        exit 1
    fi

    if ! command -v evtest >/dev/null 2>&1; then
        echo "ERROR: evtest is not installed.  sudo apt install evtest" >&2
        exit 1
    fi
    RUNNER=(stdbuf -oL evtest "$DEV")

    : > "$OUT"
    chmod 666 "$OUT"
    setsid "${RUNNER[@]}" > "$OUT" 2>&1 < /dev/null &
    echo $! > "$PIDF"
    echo "$OUT" > "$CURF"
    chmod 666 "$PIDF" "$CURF" 2>/dev/null

    sleep 1
    if ! kill -0 "$(cat "$PIDF")" 2>/dev/null; then
        echo "ERROR: the reader did not start. Log contents:" >&2
        cat "$OUT" >&2
        rm -f "$PIDF" "$CURF"
        exit 1
    fi

    echo "=================================================="
    echo " CAPTURE STARTED (pid $(cat "$PIDF"))"
    echo " device  -> $DEV"
    echo " log     -> $OUT"
    echo "=================================================="
    echo
    echo " You can now type in this very terminal."
    echo " Marker: press F8 whenever you notice a bounce."
    echo
    echo " To stop:  sudo $0 stop"
    ;;

stop)
    if [ ! -f "$PIDF" ]; then
        echo "No capture running."
        exit 0
    fi
    PID=$(cat "$PIDF")
    OUT=$(cat "$CURF" 2>/dev/null)
    kill "$PID" 2>/dev/null
    sleep 1
    kill -9 "$PID" 2>/dev/null
    rm -f "$PIDF" "$CURF"
    echo "Capture stopped."
    if [ -n "${OUT:-}" ] && [ -f "$OUT" ]; then
        echo "File   : $OUT"
        echo "Events : $(grep -c 'Event: time' "$OUT")"
        chmod 666 "$OUT"
    fi
    ;;

status)
    if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
        OUT=$(cat "$CURF" 2>/dev/null)
        echo "RUNNING (pid $(cat "$PIDF")) -> $OUT"
        [ -f "$OUT" ] && echo "Events so far: $(grep -c 'Event: time' "$OUT")"
    else
        echo "No capture running."
        rm -f "$PIDF" "$CURF" 2>/dev/null
    fi
    ;;

*)
    echo "usage: sudo $0 {start <name>|stop|status}" >&2
    exit 2
    ;;
esac
