#!/usr/bin/env python3
"""Convert an evtest log into a binary stream of struct input_event."""
import re, struct, sys

FMT = "llHHi"
RE = re.compile(
    r"Event:\s+time\s+(\d+)\.(\d+),\s+type\s+(\d+)\s+\([^)]*\),\s+"
    r"code\s+(\d+)\s+\(([^)]*)\),\s+value\s+(-?\d+)")

def main(src, dst):
    n = 0
    with open(src, encoding="utf-8", errors="replace") as fh, open(dst, "wb") as out:
        for line in fh:
            m = RE.search(line)
            if not m:
                continue
            sec, usec, etype, code, _name, value = m.groups()
            out.write(struct.pack(FMT, int(sec), int(usec.ljust(6, "0")[:6]),
                                  int(etype), int(code), int(value)))
            n += 1
    print(f"{src} -> {dst}: {n} events")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
