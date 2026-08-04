#!/usr/bin/env bash
# Remove k70debounce completely and restore the original behaviour.
#   sudo ./uninstall.sh
set -euo pipefail

UNIT=/etc/systemd/system/k70debounce.service
RUNNER=/usr/local/bin/k70debounce-run
BIN=/usr/local/bin/k70debounce
CFG_DIR=/etc/k70debounce

if [ "$(id -u)" -ne 0 ]; then
    echo "Root required. Use: sudo $0" >&2
    exit 1
fi

echo "==> Stopping and disabling the service"
systemctl disable --now k70debounce.service 2>/dev/null || true

echo "==> Removing the systemd unit"
rm -f "$UNIT"

echo "==> Removing the launcher and the binary"
rm -f "$RUNNER" "$BIN"

echo "==> Removing the configuration"
rm -rf "$CFG_DIR"

systemctl daemon-reload

echo
echo "Restored: the keyboard is back to its original behaviour."
echo "interception-tools is still installed; to remove it:"
echo "    sudo apt remove interception-tools"
