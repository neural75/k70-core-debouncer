#!/usr/bin/env python3
"""Generate a deterministic synthetic stream for the regression test.

Writes a binary file of struct input_event (24-byte layout, same as
log2bin.py: "llHHi"). The stream contains exactly 11 presses shorter than
the 26 ms threshold, so a regression run of `k70debounce --offline --hold 26`
must report 11 drops. Nothing here is captured from a real keyboard: the
original capture logs were a keylog of real typing and stay private.
"""

import struct
import sys

FMT = "llHHi"

EV_KEY, EV_SYN = 1, 0
PRESS, RELEASE = 1, 0
SYN_REPORT = 0

# key codes (linux/input-event-codes.h)
KEY_E, KEY_S, KEY_T, KEY_H, KEY_I, KEY_O, KEY_A, KEY_G, KEY_B = 18, 31, 20, 35, 23, 24, 30, 34, 48
KEY_VOLUMEUP = 115

BASE_USEC = 1_000_000  # timeline starts at t = 1.0 s

events = []


def ev(etype, code, value):
    t = events_time[-1]
    sec, usec = divmod(t, 1_000_000)
    return struct.pack(FMT, sec, usec, etype, code, value)


def advance(ms):
    events_time.append(events_time[-1] + ms * 1000)


def key(code, hold_ms):
    advance(0)
    events.append(ev(EV_KEY, code, PRESS))
    events.append(ev(EV_SYN, SYN_REPORT, 0))
    advance(hold_ms)
    events.append(ev(EV_KEY, code, RELEASE))
    events.append(ev(EV_SYN, SYN_REPORT, 0))


events_time = [BASE_USEC]

# (code, hold_ms) sequence. Human presses hold 39-72 ms and must survive;
# chatter presses hold 2-23 ms and must all be dropped. One 4 ms volume
# pulse passes through, exempted by the default ignore list.
PLAN = [
    # human                chatter (dropped)
    (KEY_E, 42), (KEY_E, 2),
    (KEY_S, 55), (KEY_S, 6),
    (KEY_T, 48),
    (KEY_H, 61), (KEY_H, 4),
    (KEY_I, 39),
    (KEY_O, 72),
    (KEY_T, 8),
    (KEY_A, 50), (KEY_A, 23),
    (KEY_G, 44),
    (KEY_I, 11),
    (KEY_E, 14),
    (KEY_B, 58),
    (KEY_S, 66), (KEY_S, 3),
    (KEY_H, 17),
    (KEY_T, 41), (KEY_T, 9),
    (KEY_O, 20),
    (KEY_E, 53),
    (KEY_I, 46),
    (KEY_H, 57),
    (KEY_VOLUMEUP, 4),      # media: ignored, passes through
]

for code, hold_ms in PLAN:
    advance(70 if code != KEY_VOLUMEUP else 40)
    key(code, hold_ms)

with open(sys.argv[1], "wb") as fh:
    for e in events:
        fh.write(e)

n_drops = sum(1 for code, hold in PLAN
              if code != KEY_VOLUMEUP and hold < 26)
print(f"{sys.argv[1]}: {len(events) // 2} key events, "
      f"{n_drops} presses below 26 ms")
