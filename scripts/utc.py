#!/usr/bin/env python3
"""
utc.py: turn the UTC epoch times found in e2sar-daos names, ids and logs into dates.

  scripts/utc.py 1793817098                       # 1793817098 --> UTC 2026-11-04 18:31:38
  scripts/utc.py 1793821780.606                   # log timestamps: seconds with a fraction
  scripts/utc.py 1793813535565250                 # ms, µs (LB predictedeventnum) or ns, by digit count
  scripts/utc.py e2sar_lb675_1793817098           # a run (container) name
  scripts/utc.py e2sar_lb675_1793817098_1793818185.evio        # a --live bundle: run start and window
  scripts/utc.py 937030334908727296.400011793820836            # a DAOS object id: kind, slot, second
  tail -3 recv.log | scripts/utc.py               # no arguments: read lines, add the date after each time
  scripts/utc.py --local 1793817098               # local time too

An integer counts as an epoch time by its digits: 10 = s, 13 = ms, 16 = µs, 19 = ns. In a
name, every run of exactly 10 digits is a second. An object id is hi.lo with
lo = K SSSS TTTTTTTTTT (kind, writer slot, second; docs/concepts/daos-layout.md#object-ids).
Standard library only.
"""
import re
import sys
from datetime import datetime, timezone

KINDS = {1: "catalog", 2: "seconds", 3: "index", 4: "data"}
UNITS = {10: 1, 13: 1e3, 16: 1e6, 19: 1e9}
LOCAL = False


def fmt(t):
    """'UTC YYYY-MM-DD HH:MM:SS[.fff]' (and local time with --local) for epoch seconds t."""
    d = datetime.fromtimestamp(t, timezone.utc)
    frac = t % 1
    s = d.strftime("UTC %Y-%m-%d %H:%M:%S") + (f".{int(round(frac * 1000)) % 1000:03d}" if frac else "")
    if LOCAL:
        s += datetime.fromtimestamp(t).astimezone().strftime("  (%Y-%m-%d %H:%M:%S %Z)")
    return s


def number(tok):
    """Epoch seconds for a bare number (10/13/16/19 digits, optionally .fraction), else None."""
    m = re.fullmatch(r"(\d+)(\.\d+)?", tok)
    if not m:
        return None
    n = len(m.group(1))
    if m.group(2):
        return float(tok) if n == 10 else None
    return int(tok) / UNITS[n] if n in UNITS else None


def explain(arg):
    """One output line for one argument."""
    m = re.fullmatch(r"(\d+)\.(\d{15})", arg)  # DAOS object id hi.lo
    if m:
        lo = int(m.group(2))
        k, slot, t = lo // 10**14, lo // 10**10 % 10**4, lo % 10**10
        what = KINDS.get(k, f"kind {k}")
        if k in (1, 2):
            return f"{arg} --> {what} object (no second)"
        return f"{arg} --> {what}, slot {slot}, second {t} = {fmt(t)}"
    t = number(arg)
    if t is not None:
        return f"{arg} --> {fmt(t)}"
    secs = re.findall(r"(?<!\d)\d{10}(?!\d)", arg)
    if secs:
        return f"{arg} --> " + ", ".join(f"{s} = {fmt(int(s))}" for s in secs)
    return f"{arg} --> no epoch time found"


def annotate(line):
    """A log line with ' [UTC …]' after each epoch time in it."""
    def rep(m):
        t = number(m.group(0))
        return m.group(0) if t is None else f"{m.group(0)} [{fmt(t)}]"
    return re.sub(r"(?<![\d.])\d{10}(?:\.\d+)?(?![\d.])|(?<![\d.])\d{13}(?:\d{3}){0,2}(?![\d.])", rep, line)


def main(argv):
    global LOCAL
    if "-h" in argv or "--help" in argv:
        print(__doc__.strip())
        return 0
    if "--local" in argv:
        LOCAL = True
        argv = [a for a in argv if a != "--local"]
    if argv:
        for a in argv:
            print(explain(a))
    else:
        for line in sys.stdin:
            sys.stdout.write(annotate(line))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
