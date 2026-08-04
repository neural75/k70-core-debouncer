#!/usr/bin/env bash
# Toggle --verbose on the installed filter, for diagnostics.
#   sudo ./debug.sh on|off
set -euo pipefail

CFG=/etc/k70debounce/config
SERVICE=k70debounce.service

if [ "$(id -u)" -ne 0 ]; then
    echo "Root required. Use: sudo $0 on|off" >&2
    exit 1
fi
[ -f "$CFG" ] || { echo "ERROR: $CFG not found - is k70debounce installed?" >&2; exit 1; }

case "${1:-}" in
on)
    sed -i 's/^VERBOSE=.*/VERBOSE=1/' "$CFG"
    echo "verbose ON"
    ;;
off)
    sed -i 's/^VERBOSE=.*/VERBOSE=0/' "$CFG"
    echo "verbose off"
    ;;
*)
    echo "usage: sudo $0 on|off" >&2
    exit 2
    ;;
esac

systemctl restart "$SERVICE"
sleep 2
echo
echo "--- configured pipeline ---"
grep -- '-g \|k70debounce' /usr/local/bin/k70debounce-run
echo
echo "--- journal (last 15 lines) ---"
journalctl -u "$SERVICE" --no-pager -n 15 2>/dev/null | grep -i k70debounce || \
    echo "(no messages)"
echo
echo "--- StatusText ---"
systemctl show "$SERVICE" -p StatusText
