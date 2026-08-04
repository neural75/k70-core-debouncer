# k70-core-debouncer

An anti-chatter keyboard filter for Linux. It cancels key presses that are
too short to be human, fixing switch chatter (a key firing twice) and
accidental grazing. Built for the Corsair K70 CORE RGB TKL, which has no
debounce setting in firmware, but it works with any keyboard.

## How it works

Every key event is delayed by a threshold (default 26 ms). If a key is
released within that window, the press is cancelled and never reaches the
system; otherwise the press goes through, unchanged and only imperceptibly
delayed.

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
