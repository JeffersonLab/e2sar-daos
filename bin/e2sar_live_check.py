#!/usr/bin/env python3
"""
e2sar_live_check.py: check the files of e2sar_daos_read --live / --live-rm against the
frames of the EVIO file that e2sar_evio_send sent.

  e2sar_live_check.py DIR RUN --digests FILE [--expected N]

  DIR        the reader's -o directory
  RUN        the run (container label): files are DIR/<RUN>_<W>.evio, .idx, _manifest.txt
  --digests  e2sar_evio_send --digests output: '<i> <sha256>' per distinct event i (with -f 1:
             per frame of the file)
  --expected events the sender sent (its "Sent:" line); default: don't check the count

Checks every file in the manifest (its SHA-256 and size), then every event in its .idx:
event number n of any dataId must be distinct event (n - 1) % E of the file, byte for byte,
where E is the number of digest lines.
Also reports duplicates, the events per dataId and the reader's _missing.txt.
Exit status 0 if everything matches.
"""

import argparse
import hashlib
import sys
from collections import Counter


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("run")
    ap.add_argument("--digests", required=True)
    ap.add_argument("--expected", type=int)
    a = ap.parse_args()

    want = {}
    for line in open(a.digests):
        frame, sha = line.split()
        want[int(frame)] = sha
    frames = len(want)

    files = ok = bad = dup = 0
    seen = set()
    per_id = Counter()
    for line in open(f"{a.dir}/{a.run}_manifest.txt"):
        f = line.split()
        sha, name, size = f[0], f[1], int(f[5])
        data = open(f"{a.dir}/{name}", "rb").read()
        files += 1
        if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
            print(f"{name}: size or SHA-256 differs from the manifest")
            bad += 1
        for row in open(f"{a.dir}/{name[:-len('.evio')]}.idx"):
            evt, data_id, off, n = map(int, row.split())
            if (evt, data_id) in seen:
                dup += 1
            seen.add((evt, data_id))
            per_id[data_id] += 1
            if hashlib.sha256(data[off:off + n]).hexdigest() == want.get((evt - 1) % frames):
                ok += 1
            else:
                bad += 1
                if bad <= 5:
                    print(f"event {evt}/{data_id} in {name}: not frame {(evt - 1) % frames} of the file")

    missing = 0
    try:
        for line in open(f"{a.dir}/{a.run}_missing.txt"):
            missing += int(line.split()[3])
    except FileNotFoundError:
        pass

    print(f"{files} files, {ok} events match their frame, {bad} bad, {dup} duplicates, "
          f"{missing} missing (_missing.txt)")
    print("events per dataId: " + ", ".join(f"{k}: {v}" for k, v in sorted(per_id.items())))
    good = bad == 0 and dup == 0 and (a.expected is None or ok == a.expected)
    if a.expected is not None:
        print(f"expected {a.expected}, got {ok}")
    print("PASS" if good else "FAIL")
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()
