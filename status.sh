#!/usr/bin/env bash
# Status of the k70debounce filter.
CFG=/etc/k70debounce/config
UNIT=/etc/systemd/system/k70debounce.service
BIN=/usr/local/bin/k70debounce
SERVICE=k70debounce.service

echo "=============================================="
echo " K70-CORE-DEBOUNCER STATUS"
echo "=============================================="

if [ -f "$CFG" ]; then
    . "$CFG"
    echo "Config     : present"
    echo "Device     : $DEVICE"
    [ -n "${DEVNAME:-}" ] && echo "             : $DEVNAME"
    echo "Threshold  : ${HOLD_MS} ms"
    echo "RT prio    : ${RT_PRIO}"
    if [ "${VERBOSE:-0}" = 1 ]; then echo "Verbose    : on"; else echo "Verbose    : off"; fi
else
    echo "Config     : MISSING"
fi

[ -f "$UNIT" ] && echo "Unit       : present" || echo "Unit       : missing"

if [ -x "$BIN" ]; then
    echo "Binary     : $($BIN --version)"
else
    echo "Binary     : MISSING"
fi

systemctl is-active --quiet "$SERVICE" 2>/dev/null \
    && echo "Service    : ACTIVE" || echo "Service    : stopped"
systemctl is-enabled "$SERVICE" >/dev/null 2>&1 \
    && echo "At boot    : enabled" || echo "At boot    : NOT enabled"

if [ -f "$UNIT" ]; then
    echo
    echo "--- running pipeline ---"
    ps -o pid,cls,rtprio,ni,comm,args -C sh -C interception -C k70debounce -C uinput 2>/dev/null \
        | grep -i 'k70debounce\|interception\|uinput' || echo "(no pipeline running)"

    echo
    echo "--- systemctl status (Status: line reported by the filter) ---"
    systemctl status "$SERVICE" --no-pager -l | grep -E 'Active:|Status:' || true

    echo
    echo "--- last drops recorded in the journal ---"
    journalctl -u "$SERVICE" --no-pager -n 200 2>/dev/null | grep 'k70debounce: dropped' | tail -6 \
        || echo "(no drops recorded)"
fi
