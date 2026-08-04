# k70-core-debouncer

An anti-chatter keyboard filter for Linux. It cancels key presses that are
too short to be human, fixing switch chatter (a key firing twice) and
accidental grazing. Built for the Corsair K70 CORE RGB TKL, which has no
debounce setting in firmware, but it works with any keyboard.

## How it works

The filter sits in an interception pipeline between your keyboard and the
system:

```
interception -g /dev/input/eventX | k70debounce --hold 26 | uinput -d /dev/input/eventX
```

1. **interception** grabs the physical keyboard: it hides it from the
   system and forwards its events to the next stage.
2. **k70debounce** is the actual filter. Every key event is delayed by a
   threshold (default 26 ms). If the key is released within that window,
   the press is cancelled and never reaches the system; otherwise it goes
   through, unchanged and only imperceptibly delayed.
3. **uinput** re-injects the filtered events through a *virtual keyboard*:
   a second device the system sees as an ordinary keyboard. Your desktop
   session reads events from that clone instead of the physical device.

The virtual keyboard is a bitmap-identical copy of the physical one, so
the system cannot tell it apart. Because events are delayed by at most the
threshold, every keystroke is sent with a constant, imperceptible latency
of a few tens of milliseconds.

Two things can drive the pipeline: the `udevmon` monitor shipped with
`interception-tools`, or — as this project does — a self-contained systemd
unit (`k70debounce.service`) that runs the pipeline directly. The systemd
approach keeps everything in one place: the service unit, the launcher and
`/etc/k70debounce/config` are all owned by this project.

## Install

```
sudo apt install interception-tools
cd src
make && make test
sudo ./install.sh
```

The installer lists the keyboards it detects and asks which one to
intercept. Both arguments are optional:

```
sudo ./install.sh 29 20    # threshold 29 ms, RT priority 20
```

The service starts immediately but is **not enabled at boot**: a reboot
restores the original behaviour. Any keyboard you did not select is left
untouched.

## Status

```
./status.sh          # configuration, service state, drops
sudo ./debug.sh on   # verbose logging (off to disable)
```

To change the threshold or device, edit `/etc/k70debounce/config` and run
`sudo systemctl restart k70debounce`.

## Inspecting with systemd and journald

```
systemctl status k70debounce               # service state, status line, drops
systemctl show k70debounce -p StatusText   # the filter's live status text
systemctl restart k70debounce              # apply a config change
systemctl stop k70debounce                 # stop the filter (keyboard reverts)

journalctl -u k70debounce                  # all log lines from the unit
journalctl -u k70debounce -f               # follow new lines as you type
journalctl -u k70debounce --since today    # today's lines only
journalctl -u k70debounce | grep dropped   # the chatter events the filter caught
```

Each dropped press is logged as `k70debounce: dropped <KEY> (hold X.X ms)`.

## Alternative: GNOME bounce keys

GNOME ships its own anti-chatter feature as an accessibility setting:

```
gsettings set org.gnome.desktop.a11y.keyboard bouncekeys-enable true
gsettings set org.gnome.desktop.a11y.keyboard bouncekeys-delay 50
```

On a GNOME Wayland session these work (mutter implements them, unlike
X11's `xkbset`). They are a convenient no-install option, but they are a
weaker filter than k70debounce:

- they reject **same-key** repeats only, so they cannot catch grazing —
  accidental quick touches of *different* keys, which k70debounce filters
  too;
- they apply only inside the desktop session: not at the login screen
  (GDM) and not to programs that read raw evdev input (games, virtual
  machines);
- they are re-disabled after a period of inactivity unless re-enabled.

Use them as a quick test or fallback; k70debounce covers the same case at
the kernel input layer, on any compositor.

## Remove

```
sudo ./uninstall.sh
```

## Tests

```
make test       # functional suite
make regress    # regression on a synthetic stream
make test-asan  # ASAN/UBSAN build
```

## License

MIT
