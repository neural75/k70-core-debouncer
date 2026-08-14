#!/usr/bin/env bash
# k70-core-debouncer installer.
#
#   sudo ./install.sh [threshold_ms] [rt_prio]
#
# Detects every keyboard on the system and lets you choose which one to
# intercept, so nothing is hardcoded to a particular model or USB ID.
# Both arguments are optional (defaults: 26 ms hold, RT priority 20).
#
# The service is started but NOT enabled at boot: a reboot restores the
# original configuration. See README.md.
set -euo pipefail

HOLD="${1:-26}"
RTPRIO="${2:-20}"

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$BASE_DIR"

BIN=/usr/local/bin/k70debounce
RUNNER=/usr/local/bin/k70debounce-run
CFG_DIR=/etc/k70debounce
CFG="$CFG_DIR/config"
UNIT=/etc/systemd/system/k70debounce.service
SERVICE=k70debounce.service

LEGACY_YAML=/etc/interception/udevmon.d/corsair-k70-minhold.yaml
LEGACY_DROPIN=/etc/systemd/system/udevmon.service.d/10-realtime.conf

# ---------------------------------------------------------------- detection

# Print "NAME|DEVICE" for every keyboard on the system. Looks first for
# stable by-id links, then by-path links (PS/2, laptops, devices without a
# by-id name), then asks the kernel which devices carry the "kbd" handler.
detect_keyboards() {
    local -a candidates=()
    local glob dev name

    # 1) Stable by-id links (most USB keyboards).
    for glob in /dev/input/by-id/*-event-kbd; do
        [ -e "$glob" ] && candidates+=("$glob")
    done

    # 2) by-path links, for keyboards with no by-id name.
    if [ "${#candidates[@]}" -eq 0 ]; then
        for glob in /dev/input/by-path/*-event-kbd; do
            [ -e "$glob" ] && candidates+=("$glob")
        done
    fi

    # 3) Last resort: anything the kernel registers with the "kbd" handler.
    if [ "${#candidates[@]}" -eq 0 ]; then
        while IFS='|' read -r name dev; do
            [ -n "$dev" ] && candidates+=("$dev")
        done < <(proc_input_keyboards)
    fi

    for dev in "${candidates[@]}"; do
        [ -e "$dev" ] || continue
        name="$(device_name "$dev")"
        printf '%s|%s\n' "$name" "$dev"
    done
}

# Parse /proc/bus/input/devices and print "NAME|/dev/input/eventN" for every
# block whose Handlers line includes "kbd".
proc_input_keyboards() {
    [ -r /proc/bus/input/devices ] || return 0
    local name="" handler=""
    local line
    while IFS= read -r line; do
        case "$line" in
            '') # end of a device block
                if [ -n "$name" ] && [ -n "$handler" ]; then
                    printf '%s|/dev/input/%s\n' "$name" "$handler"
                fi
                name=""; handler=""
                ;;
            N:\ *) name="${line#N: Name=\"}"; name="${name%\"}" ;;
            H:\ *)
                case "$line" in
                    *kbd*)
                        handler="${line#*event}"
                        handler="event${handler%% *}"
                        ;;
                esac
                ;;
        esac
    done < /proc/bus/input/devices
    if [ -n "$name" ] && [ -n "$handler" ]; then
        printf '%s|/dev/input/%s\n' "$name" "$handler"
    fi
}

device_name() {
    local dev
    dev="$(readlink -f "$1")"
    cat "/sys/class/input/$(basename "$dev")/device/name" 2>/dev/null \
        || echo "$1"
}

choose_manual() {
    while :; do
        printf 'Device path (by-id link or /dev/input/eventN): '
        read -r p
        [ -n "$p" ] && [ -e "$p" ] && [ -r "$p" ] || {
            echo "  not found or not readable: $p"
            continue
        }
        DEVICE="$p"
        DEVNAME="$(device_name "$p")"
        return
    done
}

choose_device() {
    local -a names=() links=()
    local sel p
    while IFS='|' read -r n l; do
        names+=("$n"); links+=("$l")
    done < <(detect_keyboards)

    if [ "${#links[@]}" -eq 0 ]; then
        echo "No keyboard detected under /dev/input."
        echo "Plug it in, or enter its device path manually."
        choose_manual
        return
    fi

    echo
    echo "Detected keyboards:"
    for i in "${!links[@]}"; do
        printf '  [%d] %s\n      %s\n' "$((i + 1))" "${names[$i]}" "${links[$i]}"
    done
    echo "  [m] type a device path manually (e.g. a laptop keyboard,"
    echo "      or a model without a by-id link)"
    echo
    while :; do
        printf 'Select the keyboard to intercept [1-%d, m, q]: ' "${#links[@]}"
        read -r sel
        case "$sel" in
            q) echo "Aborted."; exit 1 ;;
            m) choose_manual; return ;;
            *[!0-9]*) : ;;
            *)
                if [ "$sel" -ge 1 ] 2>/dev/null && [ "$sel" -le "${#links[@]}" ]; then
                    DEVICE="${links[$((sel - 1))]}"
                    DEVNAME="${names[$((sel - 1))]}"
                    return
                fi
                ;;
        esac
        echo "  invalid choice."
    done
}

# --------------------------------------------------------------------- main

if [ "${1:-}" = "list" ]; then
    detect_keyboards
    exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Root required. Use: sudo $0 [threshold_ms] [rt_prio]" >&2
    exit 1
fi
if [ "$RTPRIO" -gt 49 ] 2>/dev/null; then
    echo "ERROR: rt_prio must stay below 50 (the kernel IRQ threads)." >&2
    exit 1
fi

choose_device

echo
echo "==> Filtering  : $DEVNAME"
echo "    device     : $DEVICE"
echo "    threshold  : ${HOLD} ms"
echo "    RT priority: ${RTPRIO}"

echo "==> interception-tools"
if command -v interception >/dev/null 2>&1 \
    && command -v uinput >/dev/null 2>&1; then
    echo "    already present"
else
    apt-get install -y interception-tools
fi
INTERCEPTION="$(command -v interception)"
UINPUT="$(command -v uinput)"

echo "==> Checking the binary"
# We do not build here: building as root would leave root-owned files in
# the user's tree, and later `make` runs as the user would fail. Build and
# tests are done by hand, as a normal user:
#     cd src && make && make test && make regress
if [ ! -x "$SRC_DIR/k70debounce" ]; then
    echo "ERROR: $SRC_DIR/k70debounce does not exist." >&2
    echo "       Build and test first, as a normal user:" >&2
    echo "         cd $SRC_DIR && make && make test && make regress" >&2
    exit 1
fi
if [ "$SRC_DIR/k70debounce.c" -nt "$SRC_DIR/k70debounce" ]; then
    echo "ERROR: k70debounce.c is newer than the binary." >&2
    echo "       Rebuild as a normal user:  cd $SRC_DIR && make" >&2
    exit 1
fi
echo "    $SRC_DIR/k70debounce"

echo "==> Installing the binary"
install -m 755 "$SRC_DIR/k70debounce" "$BIN"
"$BIN" --version

echo "==> Writing the configuration"
mkdir -p "$CFG_DIR"
cat > "$CFG" <<EOF
# k70debounce configuration - generated by install.sh on $(date -Iseconds).
# Change a value and restart the service to apply it.
DEVICE="$DEVICE"
DEVNAME="$DEVNAME"
HOLD_MS=$HOLD
RT_PRIO=$RTPRIO
VERBOSE=0
EOF

echo "==> Installing the launcher"
cat > "$RUNNER" <<EOF
#!/bin/sh
# Launcher for the k70debounce pipeline. Reads /etc/k70debounce/config.
. $CFG
ARGS="--hold \$HOLD_MS --rt \$RT_PRIO"
[ "\$VERBOSE" = 1 ] && ARGS="\$ARGS --verbose"
$INTERCEPTION -g "\$DEVICE" | $BIN \$ARGS --notify-socket /run/systemd/notify | $UINPUT -d "\$DEVICE"
EOF
chmod 755 "$RUNNER"

echo "==> Writing the systemd unit"
cat > "$UNIT" <<EOF
# k70debounce - anti-chatter filter for the selected keyboard.
#
# Self-contained: it drives the interception pipeline directly, it does
# not go through udevmon. NotifyAccess=all lets the status line reach
# systemd even though k70debounce is a grandchild inside the pipeline.
[Unit]
Description=k70-core-debouncer anti-chatter keyboard filter
After=systemd-udevd.service

[Service]
Type=simple
ExecStart=$RUNNER
Restart=on-failure
RestartSec=2
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=$RTPRIO
LimitMEMLOCK=infinity
LimitRTPRIO=$RTPRIO
NotifyAccess=all

[Install]
WantedBy=multi-user.target
EOF

echo "==> Removing the legacy udevmon configuration (if any)"
if [ -f "$LEGACY_YAML" ]; then
    rm -f "$LEGACY_YAML"
    rm -f "$LEGACY_DROPIN"
    rmdir /etc/systemd/system/udevmon.service.d 2>/dev/null || true
    if ! compgen -G /etc/interception/udevmon.d/*.yaml >/dev/null; then
        echo "    stopping udevmon (no configuration left)"
        systemctl stop udevmon.service 2>/dev/null || true
    fi
else
    echo "    none"
fi

echo "==> Starting (NOT enabled at boot)"
systemctl daemon-reload
systemctl restart "$SERVICE"
sleep 2

if ! systemctl is-active --quiet "$SERVICE"; then
    echo "ERROR: $SERVICE did not start." >&2
    systemctl status "$SERVICE" --no-pager -l | tail -20 >&2
    exit 1
fi
echo "    $SERVICE ACTIVE"

cat <<EOF

==================================================
 DONE. The filter is active NOW.
==================================================

 Check        : $BASE_DIR/status.sh
 Disable now  : sudo systemctl stop $SERVICE
 Make it stick: sudo systemctl enable $SERVICE
 Remove all   : sudo $BASE_DIR/uninstall.sh

 It is NOT enabled at boot: if anything goes wrong, reboot.

 SAFETY NET: any keyboard that is not the one you selected
 is not filtered and stays usable at all times.
EOF
