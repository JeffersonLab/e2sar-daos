#!/usr/bin/env python3
"""Cold and warm DAOS commit latency of runs with e2sar_daos_recv --timing-csv files.

    scripts/e2sar_warm_cold_latency.py [--cold-writes N] [--csv OUT] [--plots PREFIX] RUN_DIR ...

Each RUN_DIR holds one CSV per receiver: <host>.csv (as in ~/e2sar_run/pm/pm_* and the
laptop's lux-bench/, n2n-bench/) or timing.csv (e2sar_e2e_bench.sh, ~/e2sar_run/bench/).
Columns: evt_num,data_id,bytes,chunks,thread,recv_s,commit_s,latency_ms. latency_ms is
receive -> commit: from the reassembled event handed to a DAOS thread until its last put
completes. It doesn't include the network.

Cold and warm are counted per DAOS (dequeue) thread, in the order the thread received
its events: an event is cold if it is among its thread's first --cold-writes (default 64)
events, warm after that. A receiver is cold for as long as its threads take to get
that many events each, so a slow run stays cold for longer.

Prints two tables, per receiver:
  1. latency p50/p99 (ms) by the event's place in its thread: 1st, 2nd, 3-4th, ..., 257th on
  2. cold and warm: events, p50, p90, p99, max (ms); for warm also p99.9 and the events
     over 10 ms; cold_until_s = receive time (since the receiver's first event) of the
     last cold event, i.e. how long the receiver took to warm up; and, for comparison,
     the p99 of the events received in the first 0.5 s (the time split of
     e2sar_e2e_analyze.py --warmup)
--csv writes table 2. --plots PREFIX draws two figures (needs matplotlib), from the
receivers that warm up:
  PREFIX.png        table 1: p50 and p99 against the place in the thread, one thin line
                    per receiver, the median over receivers bold
  PREFIX_time.png   every event of the first 6 s of two receivers (--time, default the
                    fastest and the slowest in Gbps of 50k events or more), cold and warm
                    marked, with the 0.5 s split
The tables need only the standard library.
"""
import argparse
import csv
import glob
import os
import statistics
import sys

BINS = [(1, 1), (2, 2), (3, 4), (5, 8), (9, 16), (17, 32), (33, 64), (65, 128), (129, 256), (257, None)]


def pct(v, p):
    """p-th percentile of the sorted list v, linearly interpolated."""
    if not v:
        return None
    k = (len(v) - 1) * p / 100
    f = int(k)
    return v[f] + (v[min(f + 1, len(v) - 1)] - v[f]) * (k - f)


def load(path):
    """[(recv_s, latency_ms, place in its thread)] in receive order."""
    rows, nbytes = [], 0
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append((float(r["recv_s"]), float(r["latency_ms"]), int(r["thread"])))
            nbytes += int(r["bytes"])
    rows.sort()
    seen = {}
    out = []
    for t, lat, th in rows:
        seen[th] = seen.get(th, 0) + 1
        out.append((t, lat, seen[th]))
    return out, len(seen), (nbytes / len(rows) if rows else 0)


def by_place(rows):
    """[(p50, p99) or None per BINS entry]."""
    out = []
    for lo, hi in BINS:
        v = sorted(l for _, l, k in rows if k >= lo and (hi is None or k <= hi))
        out.append((pct(v, 50), pct(v, 99)) if v else None)
    return out


def r3(x):
    return None if x is None else round(x, 3)


def analyze(run, host, rows, threads, cold_writes, mean_bytes):
    t0 = rows[0][0]
    cold = sorted(l for _, l, k in rows if k <= cold_writes)
    warm = sorted(l for _, l, k in rows if k > cold_writes)
    first = sorted(l for t, l, _ in rows if t - t0 < 0.5)
    rec = {"run": run, "host": host, "threads": threads, "events": len(rows),
           "window_s": round(rows[-1][0] - t0, 3), "mean_bytes": round(mean_bytes)}
    for name, v in (("cold", cold), ("warm", warm)):
        rec[f"{name}_n"] = len(v)
        for p in (50, 90, 99):
            rec[f"{name}_p{p}"] = r3(pct(v, p))
        rec[f"{name}_max"] = r3(v[-1]) if v else None
    rec["warm_p99.9"] = r3(pct(warm, 99.9))
    rec["warm_over_10ms"] = sum(l > 10 for l in warm)
    rec["cold_until_s"] = round(max(t for t, _, k in rows if k <= cold_writes) - t0, 3)
    rec["first_0.5s_p99"] = r3(pct(first, 99))
    return rec


def receivers(d):
    files = sorted(glob.glob(os.path.join(d, "ebpf*.csv")))
    if not files and os.path.exists(os.path.join(d, "timing.csv")):
        files = [os.path.join(d, "timing.csv")]
    for f in files:
        host = os.path.basename(f)[:-4]
        yield (host if host != "timing" else "recv"), f


def label(lo, hi):
    return str(lo) if hi == lo else (f"{lo}+" if hi is None else f"{lo}-{hi}")


def plt_setup():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.size": 9, "axes.spines.top": False, "axes.spines.right": False})
    return plt


COLD, WARM, REF = "#1f6fd1", "#8a8a8a", "#444444"


def plot_place(plt, places, out):
    """Latency p50/p99 against the event's place in its thread."""
    xs = list(range(len(BINS)))
    fig, ax = plt.subplots(figsize=(8, 4.2))
    for q, colour, name in ((1, COLD, "p99"), (0, WARM, "p50")):
        med = []
        for i in xs:
            v = [p[i][q] for p in places if p[i] is not None]
            med.append(statistics.median(v) if v else None)
        for p in places:
            ax.plot([i for i in xs if p[i]], [p[i][q] for i in xs if p[i]], color=colour, lw=0.6, alpha=0.25)
        ax.plot(xs, med, color=colour, lw=2.4, marker="o", ms=4, label=f"{name}, median over receivers")
    ax.axhline(1, color=REF, lw=0.8, ls="--")
    ax.text(len(BINS) - 1, 1.08, "1 ms", ha="right", va="bottom", fontsize=8, color=REF)
    ax.set_yscale("log")
    ax.set_xticks(xs, [label(lo, hi) for lo, hi in BINS])
    ax.set_xlabel("the event's place among its DAOS thread's events")
    ax.set_ylabel("receive → commit (ms)")
    ax.set_title("A DAOS thread's first ~32 writes take 15–50 ms; "
                 "p50 is 0.5 ms from its 33rd, p99 1 ms from its 129th", fontsize=10)
    ax.grid(True, axis="y", which="both", alpha=0.25)
    ax.legend(frameon=False, fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)


def gbps_of(rec):
    return rec["mean_bytes"] * 8 * rec["events"] / rec["window_s"] / 1e9


def plot_time(plt, picks, cold_writes, out, span=6.0):
    """Every event of the first `span` seconds of two receivers, cold and warm marked."""
    fig, axs = plt.subplots(len(picks), 1, figsize=(8, 2.6 * len(picks)), sharex=True)
    for ax, (rec, rows) in zip(axs, picks):
        t0 = rows[0][0]
        for want_cold, colour, name, z in ((False, WARM, "warm", 1), (True, COLD, f"cold (a thread's first {cold_writes})", 2)):
            pts = [(t - t0, l) for t, l, k in rows if t - t0 < span and (k <= cold_writes) == want_cold]
            ax.scatter([p[0] for p in pts], [p[1] for p in pts], s=2, color=colour, alpha=0.5, lw=0,
                       label=name, zorder=z, rasterized=True)
        ax.axvline(0.5, color=REF, lw=0.8, ls="--")
        ax.text(0.53, 60, "0.5 s split", fontsize=8, color=REF)
        ax.axvline(rec["cold_until_s"], color=COLD, lw=0.8)
        ax.text(rec["cold_until_s"] + 0.05, 25, f"last cold event {rec['cold_until_s']:.2f} s", fontsize=8, color=COLD)
        ax.set_title(f"{rec['run']} {rec['host']}: {gbps_of(rec):.0f} Gbps, {rec['events'] / rec['window_s']:,.0f} events/s",
                     fontsize=9, loc="left")
        ax.set_yscale("log")
        ax.set_ylim(0.2, 150)
        ax.set_ylabel("receive → commit (ms)")
        ax.grid(True, axis="y", which="major", alpha=0.25)
    axs[0].legend(frameon=False, fontsize=8, loc="upper right", markerscale=4)
    axs[-1].set_xlim(0, span)
    axs[-1].set_xlabel("receive time since the receiver's first event (s)")
    desc = ", ".join(f"{r['cold_until_s']:.1f} s at {gbps_of(r):.0f} Gbps" for r, _ in picks)
    fig.suptitle(f"A receiver is cold until every thread has had {cold_writes} events: {desc}",
                 fontsize=10, x=0.01, ha="left")
    fig.tight_layout()
    fig.savefig(out, dpi=150)


def plots(prefix, places, picks, cold_writes):
    plt = plt_setup()
    plot_place(plt, places, f"{prefix}.png")
    if len(picks) == 2:
        plot_time(plt, picks, cold_writes, f"{prefix}_time.png")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--cold-writes", type=int, default=64, help="a thread's first N events are cold (default 64)")
    ap.add_argument("--csv", help="write the cold/warm table to this CSV file")
    ap.add_argument("--time", metavar="RUN/HOST,RUN/HOST",
                    help="the two receivers of PREFIX_time.png (default: the fastest and the slowest in Gbps)")
    ap.add_argument("--plots", metavar="PREFIX",
                    help="draw PREFIX.png and PREFIX_time.png (matplotlib)")
    a = ap.parse_args()

    recs, places, kept = [], [], []
    for d in a.dirs:
        run = os.path.basename(os.path.normpath(d))
        for host, path in receivers(d):
            rows, threads, mbytes = load(path)
            if not rows:
                continue
            recs.append(analyze(run, host, rows, threads, a.cold_writes, mbytes))
            places.append(by_place(rows))
            if a.plots and recs[-1]["warm_n"] > 0:
                kept.append((recs[-1], rows))
    if not recs:
        sys.exit("no receiver CSVs found")

    def f(x, w):
        return ("-" if x is None else (f"{x:.2f}" if isinstance(x, float) else str(x))).rjust(w)

    print("1. receive -> commit p50/p99 (ms) by the event's place in its DAOS thread")
    print(f"{'run':28} {'host':9}" + "".join(f"{label(lo, hi):>12}" for lo, hi in BINS))
    for r, p in zip(recs, places):
        print(f"{r['run']:28} {r['host']:9}" +
              "".join(f"{(f'{c[0]:.1f}/{c[1]:.0f}' if c else '-'):>12}" for c in p))
    print(f"\n2. cold = a thread's first {a.cold_writes} events, warm = the rest (ms)")
    print(f"{'run':28} {'host':9} {'thr':>3} {'events':>7} {'cold n':>6} {'p50':>6} {'p99':>6} {'max':>6} "
          f"{'until s':>7} | {'warm p50':>8} {'p99':>6} {'p99.9':>6} {'max':>6} {'>10ms':>5} | {'0.5s p99':>8}")
    for r in recs:
        print(f"{r['run']:28} {r['host']:9} {r['threads']:3d} {r['events']:7d} {r['cold_n']:6d} "
              f"{f(r['cold_p50'], 6)} {f(r['cold_p99'], 6)} {f(r['cold_max'], 6)} {f(r['cold_until_s'], 7)} | "
              f"{f(r['warm_p50'], 8)} {f(r['warm_p99'], 6)} {f(r['warm_p99.9'], 6)} {f(r['warm_max'], 6)} "
              f"{r['warm_over_10ms']:5d} | {f(r['first_0.5s_p99'], 8)}")
    if a.csv:
        with open(a.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(recs[0]))
            w.writeheader()
            w.writerows(recs)
    if a.plots:
        # the time view: the fastest and the slowest receiver of 50k events or more
        if a.time:
            names = a.time.split(",")
            picks = [k for n in names for k in kept if f"{k[0]['run']}/{k[0]['host']}" == n]
        else:
            big = sorted((k for k in kept if k[0]["events"] >= 50000), key=lambda k: gbps_of(k[0]))
            picks = [big[-1], big[0]] if len(big) >= 2 else []
        plots(a.plots, [p for p, r in zip(places, recs) if r["warm_n"] > 0], picks, a.cold_writes)


if __name__ == "__main__":
    main()
