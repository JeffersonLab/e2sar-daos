#!/usr/bin/env python3
"""Analyze one e2sar_e2e_bench.sh run: throughput, per-stage latency, and digest check.

Inputs (all in the run directory):
  send.csv     e2sar_evio_send --timing-csv: evt_num,data_id,bytes,queue_ns,sent_ns (epoch ns)
  timing.csv   e2sar_daos_recv --timing-csv: evt_num,data_id,bytes,chunks,thread,recv_s,commit_s,latency_ms
               (seconds since the receiver's clock start, printed as "Clock start" in recv.log)
  digests      e2sar_evio_send --digests: <dataId> <slot> <sha256>
  fetch.txt    e2sar_daos_read --fetch (or sha256sum, dfuse sink) output: <sha256>  <prefix>_<evt>_<dataId>.evio  (...)

Latency stages per event, matched on (evt_num, data_id):
  sender   queue_ns -> sent_ns    waiting in the Segmenter queue, segmenting, sending every packet
  network  sent_ns  -> recv       wire + LB + last packet to reassembled event handed to a DAOS thread
  daos     recv     -> commit     all chunk puts and the metadata put completed
                                  (dfuse sink: open/write/close of the event's file)
  total    queue_ns -> commit
latency_steady_ms has the same stages for the events queued --warmup seconds or more after the
first one: the first ~0.2-0.4 s of a run wait up to ~150 ms while the receiver's first DAOS
writes are slow, which dominates p90/p99 of short runs. lost_queued_s lists when the lost events
were queued (seconds after the first), lost_after_warmup counts those at --warmup or later.
--profile S prints, on stderr, the events queued in each S-second slice of the run: how many,
how many were lost, and their latency (total p50/p90/max, network p50, daos p99).
The two hosts' clocks are chrony-synced (offsets ~0.1 ms), so cross-host stages are good to ~0.2 ms.

Throughput (payload bytes, Gbps = 1e9 bit/s):
  received  committed bytes of all events but the first to commit / (last commit - first commit)
  e2e       all committed bytes / (first event queued at the sender -> last commit at the receiver)
"""
import argparse
import csv
import json
import re
import sys


def pct(v, p):
    """p-th percentile of the sorted list v, linearly interpolated."""
    if not v:
        return float("nan")
    k = (len(v) - 1) * p / 100
    f = int(k)
    return v[f] + (v[min(f + 1, len(v) - 1)] - v[f]) * (k - f)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--pool", type=int, required=True, help="e2sar_evio_send --pool")
    ap.add_argument("--expected", type=int, required=True, help="events sent")
    ap.add_argument("--warmup", type=float, default=0.5, help="seconds excluded from latency_steady_ms (default 0.5)")
    ap.add_argument("--profile", type=float, metavar="S", help="print latency and loss per S-second slice of the run")
    ap.add_argument("--set", nargs="*", default=[], metavar="KEY=VALUE",
                    help="extra fields for the JSON record (numbers are converted)")
    a = ap.parse_args()
    d = a.dir

    m = re.search(r"Clock start:\s+(\d+)", open(f"{d}/recv.log").read())
    if not m:
        sys.exit("no 'Clock start' in recv.log")
    r0 = int(m.group(1)) / 1e9

    send = {}
    with open(f"{d}/send.csv") as f:
        for r in csv.DictReader(f):
            send[(int(r["evt_num"]), int(r["data_id"]))] = (int(r["queue_ns"]) / 1e9, int(r["sent_ns"]) / 1e9)

    recv = {}
    with open(f"{d}/timing.csv") as f:
        for r in csv.DictReader(f):
            recv[(int(r["evt_num"]), int(r["data_id"]))] = (
                int(r["bytes"]), r0 + float(r["recv_s"]), r0 + float(r["commit_s"]))

    # digests: every committed event must match its pool slot
    want = {}
    for line in open(f"{d}/digests"):
        did, slot, h = line.split()
        want[(int(did), int(slot))] = h
    ok = bad = 0
    for line in open(f"{d}/fetch.txt"):
        m = re.match(r"([0-9a-f]{64})\s+\S+_(\d+)_(\d+)\.evio", line)
        if not m:
            continue
        evt, did = int(m.group(2)), int(m.group(3))
        if want.get((did, evt % a.pool)) == m.group(1):
            ok += 1
        else:
            bad += 1

    out = {"expected": a.expected, "committed": len(recv), "verified": ok, "digest_mismatch": bad}
    if recv:
        commits = sorted(v[2] for v in recv.values())
        first_key = min(recv, key=lambda k: recv[k][2])
        total = sum(v[0] for v in recv.values())
        win = commits[-1] - commits[0]
        out["bytes"] = total
        out["mean_event_bytes"] = total / len(recv)
        out["commit_window_s"] = win
        out["received_gbps"] = (total - recv[first_key][0]) * 8 / win / 1e9 if win > 0 else 0
        out["received_evt_per_s"] = (len(recv) - 1) / win if win > 0 else 0
        q0 = min(v[0] for v in send.values()) if send else commits[0]
        out["e2e_s"] = commits[-1] - q0
        out["e2e_gbps"] = total * 8 / (commits[-1] - q0) / 1e9

        stages = {"sender": [], "network": [], "daos": [], "total": []}
        steady = {k: [] for k in stages}
        for k, (_, rv, cm) in recv.items():
            if k not in send:
                continue
            q, s = send[k]
            for st in (stages, steady) if q - q0 >= a.warmup else (stages,):
                st["sender"].append((s - q) * 1e3)
                st["network"].append((rv - s) * 1e3)
                st["daos"].append((cm - rv) * 1e3)
                st["total"].append((cm - q) * 1e3)
        for key, st in (("latency_ms", stages), ("latency_steady_ms", steady)):
            out[key] = {}
            for name, v in st.items():
                if not v:   # run shorter than --warmup
                    out[key][name] = None
                    continue
                v.sort()
                out[key][name] = {"p50": round(pct(v, 50), 3), "p90": round(pct(v, 90), 3),
                                  "p99": round(pct(v, 99), 3), "max": round(v[-1], 3) if v else None}
        lost = sorted(round(send[k][0] - q0, 3) for k in send if k not in recv)
        out["warmup_s"] = a.warmup
        out["lost_queued_s"] = lost
        out["lost_after_warmup"] = sum(t >= a.warmup for t in lost)
        if a.profile:
            profile(send, recv, q0, a.profile)
    out["result"] = "PASS" if out["committed"] == a.expected and ok == a.expected and bad == 0 else "FAIL"
    for kv in a.set:
        k, _, v = kv.partition("=")
        try:
            out[k] = int(v)
        except ValueError:
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    print(json.dumps(out))
    summary(out)


def profile(send, recv, q0, step):
    """Latency and loss of the events queued in each `step`-second slice, on stderr."""
    bins = {}
    for k, (q, s) in send.items():
        b = bins.setdefault(int((q - q0) / step), {"n": 0, "lost": 0, "total": [], "network": [], "daos": []})
        b["n"] += 1
        if k not in recv:
            b["lost"] += 1
            continue
        _, rv, cm = recv[k]
        b["total"].append((cm - q) * 1e3)
        b["network"].append((rv - s) * 1e3)
        b["daos"].append((cm - rv) * 1e3)
    print(f"{'queued at s':>11} {'events':>7} {'lost':>5} {'total p50':>10} {'p90':>8} {'max':>8} {'network p50':>12} {'daos p99':>9}  (ms)",
          file=sys.stderr)
    for i in sorted(bins):
        b = bins[i]
        t, nw, dq = sorted(b["total"]), sorted(b["network"]), sorted(b["daos"])
        if not t:
            print(f"{i * step:11.2f} {b['n']:7d} {b['lost']:5d}", file=sys.stderr)
            continue
        print(f"{i * step:11.2f} {b['n']:7d} {b['lost']:5d} {pct(t, 50):10.2f} {pct(t, 90):8.2f} {t[-1]:8.2f} "
              f"{pct(nw, 50):12.2f} {pct(dq, 99):9.2f}", file=sys.stderr)


def summary(a):
    """Human-readable summary on stderr."""
    g = a.get
    lines = [f"{g('tag', '')} {a['result']}  mode={g('mode')} P={g('segmenters')} s={g('sockets')} "
             f"f={g('frames_per_event')} rate={g('rate')} T={g('recv_threads')} "
             f"D={g('deq')}{'x' + str(g('inflight')) if g('inflight') not in (None, '') else ''} "
             f"{'C=' + str(g('chunk_kib')) + ' ' if g('chunk_kib') not in (None, '') else ''}sink={g('sink', 'kv')}  "
             f"committed {a['committed']}/{a['expected']}, verified {a['verified']}"]
    if "received_gbps" in a:
        lines.append(f"  received {a['received_gbps']:.2f} Gbps ({a['received_evt_per_s']:.0f} evt/s, "
                     f"{a['mean_event_bytes']:.0f} B/event)  e2e {a['e2e_gbps']:.2f} Gbps  "
                     f"sender {g('sender_goodput_gbps', 0):.2f} Gbps")
    for k, v in g("latency_ms", {}).items():
        lines.append(f"  latency {k:8s} p50 {v['p50']:9.3f}  p90 {v['p90']:9.3f}  p99 {v['p99']:9.3f}  max {v['max']} ms")
    if (a.get("latency_steady_ms") or {}).get("total"):
        v = a["latency_steady_ms"]["total"]
        lines.append(f"  after {a['warmup_s']} s: total p50 {v['p50']:.3f}  p90 {v['p90']:.3f}  p99 {v['p99']:.3f}  max {v['max']} ms, "
                     f"lost {a['lost_after_warmup']} of {len(a['lost_queued_s'])}")
    lines.append(f"  drops: rx_discards_phy {g('rx_discards_phy')} rx_out_of_buffer {g('rx_out_of_buffer')} "
                 f"UdpRcvbufErrors {g('udp_rcvbuf_errors')}")
    print("\n".join(lines), file=sys.stderr)


if __name__ == "__main__":
    main()
