#!/usr/bin/env python3
"""
Live dashboard for e2sar_daos_monitor: serves a web page that follows the monitor's JSON
lines file (one line per --every seconds) and draws it as a movie: incoming, outgoing and
missing events, what is in DAOS, and the event-number strip per dataId.

  python3 viz/daos_monitor/serve.py MON.jsonl                # follow a running monitor
  python3 viz/daos_monitor/serve.py MON.jsonl --port 8766
  python3 viz/daos_monitor/serve.py demo.jsonl --demo        # made-up data, no DAOS needed
  python3 viz/daos_monitor/serve.py demo.jsonl --demo --backfill 300   # with 5 minutes of history

The page has Live (follow the newest line) and Replay (play the file from the start, with a
slider and speed). It listens on 127.0.0.1 only. From a laptop:

  ssh -L 8766:localhost:8766 user@node     then open http://localhost:8766

Standard library only; needs Python >= 3.8.
"""
import argparse
import functools
import http.server
import json
import math
import os
import random
import threading
import time
from pathlib import Path
from urllib.parse import parse_qs, urlparse

HERE = Path(__file__).resolve().parent


class Frames:
    """The lines of the JSONL file, read incrementally as it grows."""

    def __init__(self, path):
        self.path = path
        self.lines = []
        self.pos = 0
        self.ino = None
        self.lock = threading.Lock()

    def refresh(self):
        with self.lock:
            try:
                st = os.stat(self.path)
            except FileNotFoundError:
                return
            if st.st_ino != self.ino or st.st_size < self.pos:  # replaced or truncated: start over
                self.ino, self.pos, self.lines = st.st_ino, 0, []
            if st.st_size == self.pos:
                return
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read()
            end = data.rfind(b"\n") + 1  # complete lines only
            for raw in data[:end].splitlines():
                if raw.strip():
                    self.lines.append(raw.decode(errors="replace"))
            self.pos += end

    def since(self, k):
        self.refresh()
        with self.lock:
            return self.lines[k:], len(self.lines)


class Handler(http.server.SimpleHTTPRequestHandler):
    frames = None

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/":
            self.path = "/index.html"
            return super().do_GET()
        if url.path == "/api/frames":
            q = parse_qs(url.query)
            k = max(0, int(q.get("from", ["0"])[0]))
            lines, n = self.frames.since(k)
            body = ('{"next":%d,"file":%s,"frames":[%s]}' % (n, json.dumps(str(self.frames.path)), ",".join(lines))).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        return super().do_GET()

    def log_message(self, fmt, *args):  # quiet except for errors
        if args and str(args[1]).startswith(("4", "5")):
            super().log_message(fmt, *args)


# ------------------------------------------------------------------ --demo

def demo(path, every, backfill=0.0, n_streams=18, rate=15259, bins=600, rm_after=2.0, interval=2.0):
    """Write made-up monitor lines: n_streams dataIds at `rate` events/s each, two writers,
    a --live-rm --interval 2 --rm-after 2 reader, a few lost events, and the beam off for
    10 s every 90 s. The first `backfill` seconds are written at once, dated in the past."""
    rng = random.Random(1)
    clock = time.time() - backfill  # the demo's own clock: runs ahead of real time while backfilling
    t0 = clock
    start = int(t0)
    ids = [4321 + i for i in range(n_streams)]
    span = bins
    while span < rate * 32:
        span *= 2
    width = span // bins
    ev = 0  # newest event number committed (same for every dataId: one frame per stream)
    seconds = {}  # commit second -> [events, bytes]
    total_in = total_out = bytes_in = bytes_out = 0
    gone = set()
    lost = {i: [] for i in ids}  # (evt, when) of lost event numbers still in the strip
    lost_before = {i: 0 for i in ids}  # lost ones that scrolled out of the strip: still missing
    late_total = 0
    size = 20480
    tick = 0
    with open(path, "a") as f:
        while True:
            tick += 1
            now = clock
            # a slow wobble, a burst every ~40 s, and the beam off for 10 s every 90 s
            phase = (now - t0) % 90
            r = rate * (1 + 0.15 * math.sin(now / 9)) * (1.6 if int(now) % 40 < 4 else 1) * (0.05 if 60 <= phase < 70 else 1)
            n = int(r * every)
            sec = int(now)
            new_lo, ev = ev + 1, ev + n
            seconds.setdefault(sec, [0, 0])
            seconds[sec][0] += n * n_streams
            seconds[sec][1] += n * n_streams * size
            total_in += n * n_streams
            bytes_in += n * n_streams * size
            for i in ids:  # occasional lost events, more on one crate
                for _ in range((rng.random() < 0.002) + (rng.randint(1, 4) if i == ids[7] and rng.random() < 0.02 else 0)):
                    lost[i].append((rng.randint(new_lo, ev), now))
            # the reader: complete windows rm_after intervals behind are removed
            d_out = d_out_b = 0
            for s in sorted(seconds):
                if s not in gone and s < now - (rm_after + 1) * interval - 2:
                    gone.add(s)
                    d_out += seconds[s][0]
                    d_out_b += seconds[s][1]
            total_out += d_out
            bytes_out += d_out_b
            lo = max(0, ev + 1 - span)
            # strip: event numbers map back to commit seconds at the current rate
            streams = []
            miss_all = pend_all = 0
            for i in ids:
                lost_before[i] += sum(1 for (e, w) in lost[i] if e < lo)
                lost[i] = [(e, w) for (e, w) in lost[i] if e >= lo]
                b = []
                for k in range(bins):
                    e_mid = lo + k * width + width // 2
                    t_evt = now - (ev - e_mid) / r
                    # the commit second: the last tick second at or before t_evt
                    cs = max((x for x in seconds if x <= t_evt), default=None)
                    b.append("u" if cs is None or e_mid <= 0 else "o" if cs in gone else "d")
                m, p = lost_before[i], 0
                for e, w in lost[i]:
                    k = (e - lo) // width
                    old = now - w > 5
                    b[k] = "m" if old else "p"
                    m += old
                    p += not old
                miss_all += m
                pend_all += p
                enc, k = "", 0
                while k < bins:
                    j = k
                    while j < bins and b[j] == b[k]:
                        j += 1
                    enc += f"{b[k]}{j - k}"
                    k = j
                streams.append({"id": i, "events": ev - m - p, "bytes": (ev - m - p) * size, "d_events": n, "first": 1,
                                "max": ev, "missing": m, "missing_total": m, "pending": p, "late": 0, "dups": 0, "strip_lo": lo, "strip": enc})
            in_daos_secs = [s for s in seconds if s not in gone]
            line = {
                "type": "tick", "tick": tick, "t": round(now, 3), "dt": every, "run": "demo_" + str(start), "start": start,
                "grace": 5,
                "writers": [{"slot": s, "done": False, "wm_index": sec - (s == 1 and int(now) % 60 < 6) * 3, "wm_data": sec - 1,
                             "lag": round(now - sec + (s == 1 and int(now) % 60 < 6) * 3, 3),
                             "events": total_in // 2, "bytes": bytes_in // 2} for s in (0, 1)],
                "in": {"events": total_in, "bytes": bytes_in, "d_events": n * n_streams, "d_bytes": n * n_streams * size},
                "out": {"events": total_out, "bytes": bytes_out, "d_events": d_out, "d_bytes": d_out_b},
                "daos": {"events": total_in - total_out, "bytes": bytes_in - bytes_out, "seconds": len(in_daos_secs),
                         "first": min(in_daos_secs, default=sec), "last": max(in_daos_secs, default=sec)},
                "strip": {"span": span, "bins": bins, "width": width},
                "streams": streams,
                "missing": {"total": miss_all, "events": miss_all, "pending": pend_all, "late": late_total, "dups": 0},
            }
            f.write(json.dumps(line, separators=(",", ":")) + "\n")
            f.flush()
            clock += every
            if clock > time.time():  # caught up: from here on, real time
                time.sleep(max(0.0, clock - time.time()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("jsonl", help="e2sar_daos_monitor's --out file (with --demo: the file to write)")
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--demo", action="store_true", help="write made-up monitor lines to JSONL every --every s (no DAOS)")
    ap.add_argument("--every", type=float, default=2.0, help="--demo: seconds per line")
    ap.add_argument("--backfill", type=float, default=0.0, help="--demo: first write this many seconds of past lines at once")
    a = ap.parse_args()

    path = Path(os.path.expanduser(a.jsonl)).resolve()
    if a.demo:
        threading.Thread(target=demo, args=(path, a.every, a.backfill), daemon=True).start()
    Handler.frames = Frames(path)
    handler = functools.partial(Handler, directory=str(HERE))
    with http.server.ThreadingHTTPServer(("127.0.0.1", a.port), handler) as httpd:
        print(f"DAOS monitor dashboard: http://localhost:{a.port}   following {path}   (Ctrl+C to stop)")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
