#!/usr/bin/env python3
"""
Analyse evtest logs to identify switch chatter and accidental grazing.

Primary discriminator: HOLD TIME, i.e. the press -> release interval of a
single key. It is more robust than the press -> press interval because it
does not depend on typing speed.

A human finger cannot complete a full press/release cycle in under about
30 ms, so anything below that is mechanically impossible and points at a
hardware or firmware problem.
"""

import sys
import re
from collections import defaultdict

EVENT_RE = re.compile(
    r"Event:\s+time\s+(\d+)\.(\d+),\s+type\s+(\d+)\s+\([^)]*\),\s+"
    r"code\s+(\d+)\s+\(([^)]*)\),\s+value\s+(-?\d+)"
)

EV_KEY = 1
PRESS, RELEASE, REPEAT = 1, 0, 2

# Classification thresholds (ms), chosen BEFORE looking at the data
T_CHATTER = 25.0      # below this: definitely a mechanical bounce
T_BOUNCE_REL = 45.0   # below this: plausible release bounce
T_HUMAN = 60.0        # above this: consistent with human typing


def parse(path):
    events = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = EVENT_RE.search(line)
            if not m:
                continue
            sec, usec, etype, code, name, value = m.groups()
            if int(etype) != EV_KEY:
                continue
            ts = int(sec) * 1_000_000 + int(usec.ljust(6, "0")[:6])
            events.append((ts, int(code), name, int(value)))
    return events


def histogram(values, buckets=(1, 2, 4, 8, 16, 25, 32, 45, 60, 90, 128, 256)):
    counts = defaultdict(int)
    for v in values:
        placed = False
        for b in buckets:
            if v < b:
                counts[b] += 1
                placed = True
                break
        if not placed:
            counts[None] += 1
    lines = []
    prev = 0
    total = max(len(values), 1)
    for b in buckets:
        n = counts[b]
        if n:
            bar = "#" * max(1, int(40 * n / total))
            lines.append(f"  {prev:>4}-{b:<4} ms | {n:>6}  {bar}")
        prev = b
    if counts[None]:
        n = counts[None]
        bar = "#" * max(1, int(40 * n / total))
        lines.append(f"  >={buckets[-1]:<7} ms | {n:>6}  {bar}")
    return lines


def hold_times(events):
    """Return [(hold_ms, keyname, press_index)] for every complete press."""
    down = {}
    holds = []
    for i, (ts, code, name, value) in enumerate(events):
        if value == PRESS:
            down[code] = (ts, name, i)
        elif value == RELEASE and code in down:
            t0, nm, idx = down.pop(code)
            holds.append(((ts - t0) / 1000.0, nm, idx))
    return holds


def recommend_threshold(holds):
    """
    Look for the gap between the anomalous and the human populations and
    suggest a threshold in the middle of it.
    """
    vals = sorted(h[0] for h in holds)
    if len(vals) < 50:
        return None, "not enough samples (fewer than 50 presses)"

    anomalous = [v for v in vals if v < 30.0]
    human = [v for v in vals if v >= 30.0]
    if not anomalous:
        return None, "no press below 30 ms: no chatter detected"
    if not human:
        return None, "no plausible human press: check the capture"

    gap_lo, gap_hi = max(anomalous), min(human)
    if gap_hi - gap_lo < 4.0:
        return None, (f"the two populations nearly touch "
                      f"({gap_lo:.1f} / {gap_hi:.1f} ms): risky, do not filter")
    suggested = round((gap_lo + gap_hi) / 2.0)
    return suggested, (f"gap between {gap_lo:.1f} ms (worst anomaly) and "
                       f"{gap_hi:.1f} ms (shortest human press)")


def main(paths):
    all_events = []
    for p in paths:
        ev = parse(p)
        print(f"[{p}] EV_KEY events: {len(ev)}")
        all_events.extend(ev)

    if not all_events:
        print("\nNO EVENTS FOUND. The log is empty or has an unexpected format.")
        return 1

    all_events.sort(key=lambda e: e[0])
    t0, t1 = all_events[0][0], all_events[-1][0]
    print(f"Session length: {(t1 - t0) / 1e6:.1f} s\n")

    markers = [e for e in all_events if e[2] == "KEY_F8" and e[3] == PRESS]
    marker_ts = [e[0] for e in markers]

    # ---------------- hold times: the primary discriminator ----------------
    holds = hold_times(all_events)
    hvals = sorted(h[0] for h in holds)

    print("=" * 62)
    print("HOLD TIME DISTRIBUTION (press -> release)")
    print("=" * 62)
    if hvals:
        print(f"  complete presses : {len(hvals)}")
        print(f"  minimum          : {hvals[0]:.1f} ms")
        print(f"  1st percentile   : {hvals[int(len(hvals) * 0.01)]:.1f} ms")
        print(f"  median           : {hvals[len(hvals) // 2]:.1f} ms")
        print()
        for line in histogram([v for v in hvals if v < 512]):
            print(line)

    suspicious = sorted(h for h in holds if h[0] < 30.0)
    print()
    print("=" * 62)
    print("IMPOSSIBLE PRESSES (hold < 30 ms)")
    print("=" * 62)
    if not suspicious:
        print("  none")
    else:
        for held, nm, _ in suspicious:
            print(f"  {nm:<16} hold={held:6.1f} ms")
        print(f"\n  total: {len(suspicious)} out of {len(hvals)} presses "
              f"({100.0 * len(suspicious) / max(len(hvals), 1):.3f} %)")

    # ---------------- recommended threshold ----------------
    print()
    print("=" * 62)
    print("RECOMMENDED THRESHOLD")
    print("=" * 62)
    suggested, reason = recommend_threshold(holds)
    if suggested is None:
        print(f"  none: {reason}")
        print("  => installing the filter is probably NOT worth it.")
    else:
        print(f"  --hold {suggested}")
        print(f"  reason: {reason}")

    # ---------------- press -> press, same key ----------------
    last = {}
    deltas = defaultdict(list)
    for ts, code, name, value in all_events:
        if value == REPEAT:
            continue
        key = (code, value)
        if key in last:
            deltas[(code, name, value)].append((ts - last[key]) / 1000.0)
        last[key] = ts

    press_deltas = []
    for (code, name, value), ds in deltas.items():
        if value == PRESS:
            press_deltas.extend(ds)

    print()
    print("=" * 62)
    print("PRESS -> PRESS INTERVALS, SAME KEY")
    print("=" * 62)
    for line in histogram([d for d in press_deltas if d < 512]):
        print(line)

    n_chatter = sum(1 for d in press_deltas if d < T_CHATTER)
    n_bounce = sum(1 for d in press_deltas if T_CHATTER <= d < T_BOUNCE_REL)
    n_grey = sum(1 for d in press_deltas if T_BOUNCE_REL <= d < T_HUMAN)

    print()
    print(f"  < {T_CHATTER:g} ms  (definite mechanical bounce) : {n_chatter}")
    print(f"  {T_CHATTER:g}-{T_BOUNCE_REL:g} ms (plausible release bounce) : {n_bounce}")
    print(f"  {T_BOUNCE_REL:g}-{T_HUMAN:g} ms (grey area)                  : {n_grey}")
    print(f"  repetitions analysed                        : {len(press_deltas)}")

    # ---------------- legitimate double letters ----------------
    presses = [(ts, code, name) for ts, code, name, v in all_events if v == PRESS]
    consecutive = []
    for i in range(1, len(presses)):
        if presses[i][1] == presses[i - 1][1]:
            consecutive.append(((presses[i][0] - presses[i - 1][0]) / 1000.0,
                                presses[i][2]))
    legit = [d for d, _ in consecutive if d >= 60.0]
    if legit:
        print()
        print("=" * 62)
        print("LEGITIMATE DOUBLE LETTERS (same key twice in a row)")
        print("=" * 62)
        print(f"  samples : {len(legit)}")
        print(f"  minimum : {min(legit):.1f} ms")
        print(f"  median  : {sorted(legit)[len(legit) // 2]:.1f} ms")
        print("  A debounce filter must stay well below the minimum;")
        print("  a min-hold filter is not affected by this at all.")

    # ---------------- special keys ----------------
    print()
    print("=" * 62)
    print("SPECIAL / PHANTOM KEYS")
    print("=" * 62)
    special = defaultdict(int)
    for ts, code, name, value in all_events:
        if value == PRESS and name in (
            "KEY_SCROLLLOCK", "KEY_FN", "KEY_NUMLOCK", "KEY_SYSRQ",
            "KEY_PAUSE", "KEY_INSERT", "KEY_COMPOSE", "KEY_MENU",
        ):
            special[name] += 1
    if special:
        for k, v in sorted(special.items(), key=lambda x: -x[1]):
            print(f"  {k:<20} {v}")
    else:
        print("  none detected")

    # ---------------- markers ----------------
    print()
    print("=" * 62)
    print(f"F8 MARKERS: {len(marker_ts)}")
    print("=" * 62)
    if marker_ts:
        print("  Anomalies in the 3 s before each marker:\n")
        for i, mt in enumerate(marker_ts, 1):
            near = [f"{nm} hold={h:.1f}ms"
                    for h, nm, idx in holds
                    if mt - 3_000_000 <= all_events[idx][0] <= mt and h < 30.0]
            print(f"  #{i}: {', '.join(near) if near else 'nothing anomalous'}")

    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: analyze.py <log> [log2 ...]")
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
