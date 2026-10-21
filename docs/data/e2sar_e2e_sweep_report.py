#!/usr/bin/env python3
"""Summarize a rate sweep: one JSON line per e2sar_e2e_bench.sh run, grouped by sweep and rate.

Input: a .jsonl file, one e2sar_e2e_analyze.py result per line, plus the fields that
e2sar_e2e_sweep.sh adds (tag, rate, sink, sweep, rx_discards_phy, ...).
Example: docs/data/e2e_sweep_2026-10-20.jsonl (54 runs, "sweep" = kv, kv-staggered-start, pil4dfs).

Tables (Markdown, as in docs/E2E_BENCHMARK.md section 7), one row per (sweep, rate):
  summary   lossless runs, received and E2E Gbps (min-max), lost events per run,
            runs with no loss after the warm-up, NIC port discards per run
  steady    latency per stage (p50, p99) of the events queued after the warm-up (latency_steady_ms)
  warmup    total latency of all events (latency_ms) next to the steady state
Ranges are min-max over the runs of the group.

  docs/data/e2sar_e2e_sweep_report.py docs/data/e2e_sweep_2026-10-20.jsonl
  docs/data/e2sar_e2e_sweep_report.py FILE --sweep kv --sweep kv-staggered-start --table summary
  docs/data/e2sar_e2e_sweep_report.py FILE --merge            # kv and kv-staggered-start as one group per sink
  docs/data/e2sar_e2e_sweep_report.py FILE --csv runs.csv     # one flat row per run, for a spreadsheet or plot
  docs/data/e2sar_e2e_sweep_report.py FILE --plot sweep.png   # 4 panels vs rate, one dot per run (needs matplotlib)
  docs/data/e2sar_e2e_sweep_report.py FILE --only lossless --runs   # list the lossless runs
  docs/data/e2sar_e2e_sweep_report.py FILE --plot-loss loss.png     # lossless throughput; loss rate vs throughput
"""
import argparse
import csv
import json
import sys
from collections import defaultdict

STAGES = ("sender", "network", "daos", "total")


def load(path):
    runs = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                runs.append(json.loads(line))
            except json.JSONDecodeError as e:
                sys.exit(f"{path}:{n}: {e}")
    return runs


def lost(r):
    return r["expected"] - r["committed"]


def lossless(r):
    """Every expected event committed and verified."""
    return lost(r) == 0 and r["verified"] == r["committed"] and r.get("digest_mismatch", 0) == 0


def loss_pct(r):
    return 100.0 * lost(r) / r["expected"]


def sweep_of(r):
    return r.get("sweep", r.get("sink", "?"))


def table_runs(runs):
    """One row per run."""
    out = ["| Tag | Sweep | Rate | E2E Gbps | Received Gbps | Lost | Loss rate | NIC discards |",
           "|---|---|---|---|---|---|---|---|"]
    for r in sorted(runs, key=lambda r: (sweep_of(r), r["rate"], r["tag"])):
        out.append(f"| {r['tag']} | {sweep_of(r)} | {r['rate']} | {r['e2e_gbps']:.2f} "
                   f"| {r['received_gbps']:.2f} | {lost(r)} | {loss_pct(r):.3f}% "
                   f"| {r.get('rx_discards_phy', '–')} |")
    return out


def rng(values, fmt="{:.2f}"):
    """'a-b', or 'a' when all values print the same."""
    values = [v for v in values if v is not None]
    if not values:
        return "–"
    lo, hi = fmt.format(min(values)), fmt.format(max(values))
    return lo if lo == hi else f"{lo}–{hi}"


def group(runs, merge):
    """{(group name, rate): [runs]} in file order of the group names, then by rate."""
    groups = defaultdict(list)
    order = []
    for r in runs:
        name = r.get("sink", "?") if merge else r.get("sweep", r.get("sink", "?"))
        if name not in order:
            order.append(name)
        groups[(name, r["rate"])].append(r)
    return sorted(groups.items(), key=lambda kv: (order.index(kv[0][0]), kv[0][1]))


def table_summary(groups):
    out = ["| Sweep | Rate | Lossless runs | Received | E2E | Lost events per run "
           "| Runs with no loss after warm-up | NIC discards per run |",
           "|---|---|---|---|---|---|---|---|"]
    for (name, rate), rs in groups:
        n = len(rs)
        ok = sum(1 for r in rs if lost(r) == 0 and r.get("digest_mismatch", 0) == 0)
        clean_after = sum(1 for r in rs if r.get("lost_after_warmup", lost(r)) == 0)
        losses = ", ".join(str(x) for x in sorted(lost(r) for r in rs))
        out.append(f"| {name} | {rate} | {ok} / {n} "
                   f"| {rng([r['received_gbps'] for r in rs])} "
                   f"| {rng([r['e2e_gbps'] for r in rs])} "
                   f"| {losses} | {clean_after} / {n} "
                   f"| {rng([r.get('rx_discards_phy') for r in rs], '{:d}')} |")
    return out


def table_steady(groups):
    out = ["| Sweep | Rate | Stage | p50 | p99 | max |", "|---|---|---|---|---|---|"]
    for (name, rate), rs in groups:
        for i, st in enumerate(STAGES):
            lat = [r["latency_steady_ms"][st] for r in rs if "latency_steady_ms" in r]
            head = f"| {name} | {rate} " if i == 0 else "| | "
            bold = "**" if st == "total" else ""
            out.append(f"{head}| {bold}{st}{bold} "
                       f"| {rng([l['p50'] for l in lat])} "
                       f"| {rng([l['p99'] for l in lat])} "
                       f"| {rng([l['max'] for l in lat])} |")
    return out


def table_warmup(groups):
    out = ["| Sweep | Rate | All events: total p50 | p90 | p99 | max "
           "| Steady: total p50 | p99 |",
           "|---|---|---|---|---|---|---|---|"]
    for (name, rate), rs in groups:
        a = [r["latency_ms"]["total"] for r in rs]
        s = [r["latency_steady_ms"]["total"] for r in rs if "latency_steady_ms" in r]
        out.append(f"| {name} | {rate} "
                   f"| {rng([x['p50'] for x in a])} | {rng([x['p90'] for x in a], '{:.1f}')} "
                   f"| {rng([x['p99'] for x in a], '{:.0f}')} | {rng([x['max'] for x in a], '{:.0f}')} "
                   f"| {rng([x['p50'] for x in s])} | {rng([x['p99'] for x in s])} |")
    return out


def write_csv(runs, path):
    """One row per run; nested latency dicts become e.g. steady_total_p99."""
    rows = []
    for r in runs:
        row = {k: v for k, v in r.items() if not isinstance(v, (dict, list))}
        row["lost"] = lost(r)
        row["lost_queued_s"] = " ".join(str(x) for x in r.get("lost_queued_s", []))
        for key, prefix in (("latency_ms", "all"), ("latency_steady_ms", "steady")):
            for st, ps in r.get(key, {}).items():
                for p, v in ps.items():
                    row[f"{prefix}_{st}_{p}"] = v
        rows.append(row)
    fields = []
    for row in rows:
        fields += [k for k in row if k not in fields]
    f = sys.stdout if path == "-" else open(path, "w", newline="", encoding="utf-8")
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(rows)
    if f is not sys.stdout:
        f.close()

# One slot per sweep, in fixed order (validated categorical palette, light mode).
COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#6250d6", "#e87ba4"]
MARKERS = ["o", "s", "^", "D", "v"]


def plot(runs, path):
    """Four panels against the sender rate, one dot per run, one color per sweep:
    E2E Gbps, lost events, latency after the warm-up (p50 and p99), NIC port discards."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    names = []
    for r in runs:
        if r.get("sweep", r.get("sink")) not in names:
            names.append(r.get("sweep", r.get("sink")))
    rates = sorted({r["rate"] for r in runs})
    step = min((b - a for a, b in zip(rates, rates[1:])), default=5)
    dx = {n: (i - (len(names) - 1) / 2) * step * 0.15 for i, n in enumerate(names)}

    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False,
                         "axes.edgecolor": "#8a8984", "axes.labelcolor": "#52514e",
                         "xtick.color": "#52514e", "ytick.color": "#52514e",
                         "axes.titlesize": 11, "axes.titleweight": "bold"})
    fig, ax = plt.subplots(2, 2, figsize=(12, 8.5), facecolor="#fcfcfb")
    (a_tp, a_lost), (a_lat, a_disc) = ax
    for a in ax.flat:
        a.set_facecolor("#fcfcfb")
        a.grid(True, color="#e4e3df", linewidth=0.8)
        a.set_axisbelow(True)
        a.set_xticks(rates)
        a.set_xlabel("Sender rate (Gbps)")

    lo, hi = rates[0] - step / 2, rates[-1] + step / 2
    a_tp.plot([lo, hi], [lo, hi], color="#8a8984", linewidth=1, linestyle="--", label="= rate")
    for i, n in enumerate(names):
        rs = [r for r in runs if r.get("sweep", r.get("sink")) == n]
        c, m = COLORS[i % len(COLORS)], MARKERS[i % len(MARKERS)]
        x = [r["rate"] + dx[n] for r in rs]
        kw = dict(color=c, marker=m, s=36, edgecolors="#fcfcfb", linewidths=1, zorder=3)
        a_tp.scatter(x, [r["e2e_gbps"] for r in rs], label=n, **kw)
        # Lossless runs drawn hollow on the zero line.
        ok = [lost(r) == 0 for r in rs]
        a_lost.scatter([xi for xi, o in zip(x, ok) if not o],
                       [lost(r) for r, o in zip(rs, ok) if not o], label=n, **kw)
        a_lost.scatter([xi for xi, o in zip(x, ok) if o], [0] * sum(ok), marker=m, s=36,
                       facecolors="none", edgecolors=c, linewidths=1.5, zorder=3)
        st = [r.get("latency_steady_ms", r["latency_ms"])["total"] for r in rs]
        a_lat.scatter(x, [s_["p50"] for s_ in st], label=f"{n} p50", **kw)
        a_lat.scatter(x, [s_["p99"] for s_ in st], marker=m, s=36, facecolors="none",
                      edgecolors=c, linewidths=1.5, zorder=3, label=f"{n} p99")
        a_disc.scatter(x, [r.get("rx_discards_phy", 0) for r in rs], label=n, **kw)

    a_tp.set_title("End-to-end throughput")
    a_tp.set_ylabel("E2E Gbps (sender queue -> DAOS commit)")
    a_tp.legend(frameon=False, loc="upper left")
    a_lost.set_title("Lost events per run (hollow = lossless)")
    a_lost.set_ylabel("Events lost (of %d)" % runs[0]["expected"])
    a_lost.set_yscale("symlog", linthresh=10)
    a_lost.set_ylim(bottom=-0.5)
    a_lost.legend(frameon=False, loc="upper left")
    warm = runs[0].get("warmup_s", 0)
    a_lat.set_title(f"Total latency after the first {warm} s (filled p50, hollow p99)")
    a_lat.set_ylabel("ms (log)")
    a_lat.set_yscale("log")
    a_lat.legend(frameon=False, fontsize=8, ncol=2, loc="center right")
    a_disc.set_title("NIC port discards per run (rx_discards_phy)")
    a_disc.set_ylabel("Frames")
    a_disc.set_yscale("symlog", linthresh=10)
    a_disc.set_ylim(bottom=-0.5)
    a_disc.legend(frameon=False, loc="upper left")
    allv = all(r["verified"] == r["committed"] and r.get("digest_mismatch", 0) == 0 for r in runs)
    fig.suptitle(f"Node-to-node rate sweep: {len(runs)} runs, {runs[0]['expected']:,} events each"
                 + (" (every committed event verified by SHA-256)" if allv else ""),
                 fontsize=12, color="#0b0b0b")
    fig.tight_layout()
    fig.savefig(path, dpi=130, facecolor=fig.get_facecolor())



def plot_loss(runs, path):
    """Two panels, one dot per run, one color per sweep:
    left, E2E Gbps of the lossless runs against the sender rate;
    right, loss rate (lost / expected) of the other runs against their E2E Gbps."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    names = []
    for r in runs:
        if sweep_of(r) not in names:
            names.append(sweep_of(r))
    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False,
                         "axes.edgecolor": "#8a8984", "axes.labelcolor": "#52514e",
                         "xtick.color": "#52514e", "ytick.color": "#52514e",
                         "axes.titlesize": 11, "axes.titleweight": "bold"})
    fig, (a_ok, a_loss) = plt.subplots(1, 2, figsize=(12, 4.8), facecolor="#fcfcfb")
    for a in (a_ok, a_loss):
        a.set_facecolor("#fcfcfb")
        a.grid(True, color="#e4e3df", linewidth=0.8)
        a.set_axisbelow(True)

    ok = [r for r in runs if lossless(r)]
    bad = [r for r in runs if not lossless(r)]
    rates = sorted({r["rate"] for r in runs})
    lo, hi = rates[0] - 2.5, rates[-1] + 2.5
    a_ok.plot([lo, hi], [lo, hi], color="#8a8984", linewidth=1, linestyle="--", label="= rate")
    for i, n in enumerate(names):
        kw = dict(color=COLORS[i % len(COLORS)], marker=MARKERS[i % len(MARKERS)], s=48,
                  edgecolors="#fcfcfb", linewidths=1, zorder=3)
        rs = [r for r in ok if sweep_of(r) == n]
        if rs:
            a_ok.scatter([r["rate"] for r in rs], [r["e2e_gbps"] for r in rs],
                         label=f"{n} ({len(rs)})", **kw)
        rs = [r for r in bad if sweep_of(r) == n]
        if rs:
            a_loss.scatter([r["e2e_gbps"] for r in rs], [loss_pct(r) for r in rs],
                           label=f"{n} ({len(rs)})", **kw)

    a_ok.set_xticks(rates)
    a_ok.set_xlim(lo, hi)
    a_ok.set_xlabel("Sender rate (Gbps)")
    a_ok.set_ylabel("E2E Gbps (sender queue -> DAOS commit)")
    a_ok.set_title(f"Lossless runs: {len(ok)} of {len(runs)}")
    a_ok.legend(frameon=False, loc="upper left")

    a_loss.set_xlabel("E2E Gbps (sender queue -> DAOS commit)")
    a_loss.set_ylabel("Loss rate, % of events (log)")
    a_loss.set_yscale("log")
    a_loss.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}%"))
    a_loss.set_title(f"Runs with loss: {len(bad)} of {len(runs)}")
    a_loss.legend(frameon=False, loc="upper left")
    fig.suptitle(f"Rate sweep, {runs[0]['expected']:,} events per run", fontsize=12,
                 color="#0b0b0b")
    fig.tight_layout()
    fig.savefig(path, dpi=130, facecolor=fig.get_facecolor())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 2)[2])
    ap.add_argument("jsonl")
    ap.add_argument("--sweep", action="append",
                    help="only these sweeps (repeatable), e.g. kv, pil4dfs")
    ap.add_argument("--rate", type=int, action="append", help="only these rates (repeatable)")
    ap.add_argument("--merge", action="store_true",
                    help="group by sink instead of sweep (e.g. kv + kv-staggered-start)")
    ap.add_argument("--table", choices=("summary", "steady", "warmup", "all"), default="all")
    ap.add_argument("--csv", metavar="FILE", help="write one flat row per run ('-' = stdout) and exit")
    ap.add_argument("--only", choices=("lossless", "lossy"),
                    help="only runs with every event committed and verified, or only the others")
    ap.add_argument("--runs", action="store_true", help="print one row per run instead of the tables")
    ap.add_argument("--plot-loss", metavar="FILE",
                    help="save lossless throughput and loss rate vs throughput (png, pdf, svg) and exit")
    ap.add_argument("--plot", metavar="FILE", help="save a 4-panel figure (png, pdf, svg) and exit")
    a = ap.parse_args()

    runs = load(a.jsonl)
    if a.sweep:
        runs = [r for r in runs if r.get("sweep") in a.sweep]
    if a.rate:
        runs = [r for r in runs if r["rate"] in a.rate]
    if a.plot_loss:  # needs both kinds of run, so before --only
        if not runs:
            sys.exit("no runs match")
        plot_loss(runs, a.plot_loss)
        return
    if a.only:
        runs = [r for r in runs if lossless(r) == (a.only == "lossless")]
    if not runs:
        sys.exit("no runs match")
    if a.runs:
        print("\n".join(table_runs(runs)))
        return

    if a.csv:
        write_csv(runs, a.csv)
        return
    if a.plot:
        plot(runs, a.plot)
        return

    groups = group(runs, a.merge)
    sections = {"summary": ("Throughput (Gbps) and loss", table_summary),
                "steady": ("Latency per stage after the warm-up (ms)", table_steady),
                "warmup": ("Total latency, all events vs after the warm-up (ms)", table_warmup)}
    warm = sorted({r.get("warmup_s") for r in runs if r.get("warmup_s") is not None})
    print(f"{len(runs)} runs from {a.jsonl}" + (f", warm-up {warm} s" if warm else "") + "\n")
    for name in (sections if a.table == "all" else [a.table]):
        title, fn = sections[name]
        print(f"### {title}\n")
        print("\n".join(fn(groups)) + "\n")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:  # e.g. piped into head
        sys.stderr.close()
