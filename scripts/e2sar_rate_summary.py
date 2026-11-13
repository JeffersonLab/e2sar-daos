#!/usr/bin/env python3
"""Draw every measured rate and its event loss, one row per setting.

    scripts/e2sar_rate_summary.py docs/images/e2sar_rate_summary.svg

The numbers are typed in from the result pages in docs/results (named in
each row's `src`); see docs/results/2026-11-12-rate-summary.md. One panel per
setting, one marker per run: x is the rate it reached (received, or sent where
only that is known), y the share of its events lost (log), with lossless and
crashed runs in strips below the log axis. Needs matplotlib.
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

CRASH = None  # the run gave no result (receiver died)


def frac(rate, lost_list, n):
    """[(rate, lost/n), ...] for several runs at one rate."""
    return [(rate, lost / n) for lost in lost_list]


def perf(rate, *lost, x=None):
    """e2sar_perf section 10: rate x 1000 events of 1 MiB per run."""
    return [(x or rate, CRASH if l is CRASH else l / (rate * 1000)) for l in lost]


# (group, label, best lossless text, runs)
ROWS = [
    ("E2SAR only: e2sar_perf at both ends, no DAOS (1 MiB, 16 sockets → 32 threads, ebpf2201 → ebpf2203)", None, None, None),
    (None, "Node-to-node, plain (10-26)", "20 (1 of 2, other crashed)",
     perf(10, 0, CRASH) + perf(20, 0, CRASH) + perf(30, 17, 11) + perf(40, 48, 48) + perf(60, 46, 32)
     + perf(80, 47, CRASH, x=75) + perf(90, 41, 48, x=87)),
    (None, "Node-to-node, recvmmsg 1024 (10-26)", "20 (1 of 2, other crashed)",
     perf(10, 0, 0) + perf(20, CRASH, 0) + perf(30, 9, 46) + perf(40, 840, 52) + perf(60, CRASH, 45)
     + perf(80, 1951, 60, x=76) + perf(90, 1226, 46, x=88)),
    (None, "LB 86, plain (10-28)", "10 (2 of 2)",
     perf(10, 0, 0) + perf(20, 78, 40) + perf(30, 16, 54) + perf(40, 16, 4) + perf(60, 15, 26)
     + perf(80, 34, 17, x=78.5) + perf(90, 25, 27, x=89.6)),
    (None, "LB 86, recvmmsg 1024 (10-28)", "10 (2 of 2); 30 (1 of 2)",
     perf(10, 0, 0) + perf(20, 42, 77) + perf(30, 5, 0) + perf(40, 6, 5) + perf(60, 21, 266)
     + perf(80, 27, 35, x=80.4) + perf(90, 52, 35, x=89.7)),

    ("Node-to-node into DAOS (e2sar_daos_recv on ebpf2203)", None, None, None),
    (None, "95 KB, 16 Segmenters (10-19)", "25 (1 of 2)",
     frac(20.09, [2, 15, 0], 1e5) + frac(25.2, [0, 4], 1e5) + frac(30.2, [8, 26], 1e5) + frac(35.3, [5, 14], 1e5)),
    (None, "95 KB rate sweep, 6 runs per rate (10-20)", "30 (1 of 6)",
     frac(20.1, [0, 1, 3, 5, 6, 8], 1e5) + frac(25.2, [0, 0, 2, 4, 4, 4], 1e5)
     + frac(30.3, [0, 4, 5, 9, 11, 61], 1e5) + frac(35.4, [3, 4, 4, 10, 10, 19], 1e5)
     + frac(40.5, [5, 7, 8, 9, 10, 33], 1e5) + frac(45.7, [8, 9, 14, 18, 19, 19], 1e5)
     + frac(50.9, [7, 12, 12, 18, 24, 30], 1e5)),
    (None, "1 MiB, 8 streams × 8 sockets (10-29)", "none",
     [(20, 3 / 35760), (40, 3 / 71520), (59.9, 35 / 107288), (79.8, 35 / 143048)]),
    (None, "1 MiB, 8 streams × 2 sockets, --deq 8 (10-29)", "40 (1 of 1)",
     [(40, 0), (60, 15 / 107288), (80, 15 / 143048)]),

    ("Through the LB into DAOS", None, None, None),
    (None, "95 KB, 16 Segmenters, LB 638 (10-19)", "35 (2 of 2)",
     frac(20.0, [0] * 6, 1e5) + frac(25.1, [0, 17], 1e5) + frac(30.2, [0, 10], 1e5) + frac(35.3, [0, 0], 1e5)
     + frac(40.4, [11, 177], 1e5) + frac(45.5, [5, 7], 1e5)),
    (None, "95 KB rate sweep, LB 640 (10-21)", "25 (1 of 2)",
     frac(20.09, [0], 1e5) + frac(25.15, [0, 1], 1e5) + frac(30.24, [3, 6], 1e5) + frac(35.35, [3, 16], 1e5)
     + frac(40.44, [10, 20], 1e5) + frac(45.58, [4, 9], 1e5)),
    (None, "2 GB files, e2sar_ft, LB 638 (10-19) ¹", "25.8 (2 of 4)",
     [(7.12, 0), (7.12, 0), (8.59, 0), (11.45, 0), (11.45, 0), (11.45, 0),
      (25.77, 0), (25.77, 1.0), (25.77, 0), (25.77, 1.0), (22.91, 0.75), (34.36, 1.0), (34.36, 0.5)]),
    (None, "1 MiB from Perlmutter, 2 receivers (10-27)", "none",
     [(9.99, 1.28e-4), (19.94, 3.39e-4), (39.65, 4.9e-3), (78.32, 1.7e-5), (79.79, 9.7e-5)]),
    (None, "100 KB EVIO, LB 682, 1 receiver (11-10)", "10",
     [(5, 0), (10, 0), (15, 21 / 1657756), (20, 101 / 2210344)]),
    (None, "  same with --ramp 10 (11-10)", "15",
     [(5, 0), (10, 0), (15, 0), (20, 3 / 2210344)]),
    (None, "External senders, LB 686, 2 receivers (11-12) ²", "none",
     [(3.17, 3.55e-3), (20.90, 2.75e-5), (41.9, 6.13e-4), (40.83, 1.09e-3)]),
]

BLUE = "#2a78d6"
INK, INK2, MUTED, GRID, AXIS = "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"
# log loss axis; lossless and crashed runs sit in strips below it
Y_ZERO, Y_CRASH, Y_BREAK, Y_MIN, Y_MAX = 2.2e-7, 6e-8, 6e-7, 3e-8, 2.0
NCOL = 8


def spread(runs):
    """Offset runs that land on the same point (lossless or crashed at one rate) sideways."""
    seen, k, out = {}, {}, []
    for x, f in runs:
        key = (round(x), "c" if f is CRASH else f == 0)
        if f is CRASH or f == 0:
            seen[key] = seen.get(key, 0) + 1
    for x, f in runs:
        key = (round(x), "c" if f is CRASH else f == 0)
        if key not in seen:
            out.append((x, f))
            continue
        m, j = seen[key], k.get(key, 0)
        k[key] = j + 1
        out.append((x + (j - (m - 1) / 2) * 1.0, f))
    return out


def panel(ax, label, best, runs, left, bottom):
    ax.set_xscale("linear")
    ax.set_yscale("log")
    ax.set_xlim(0, 95)
    ax.set_ylim(Y_MIN, Y_MAX)
    ax.set_facecolor("white")
    for y in (1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 1):
        ax.axhline(y, color=GRID, lw=0.7, zorder=0)
    ax.axhline(Y_BREAK, color=AXIS, lw=0.8, ls=(0, (3, 2)), zorder=0)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(AXIS)
    ax.tick_params(colors=INK2, length=0, labelsize=8.5)
    ax.set_xticks([0, 20, 40, 60, 80])
    ax.set_yticks([Y_CRASH, Y_ZERO, 1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 1])
    ax.set_yticklabels(["crash", "0", "10⁻⁶", "10⁻⁵", "10⁻⁴", "10⁻³", "1%", "10%", "100%"])
    ax.minorticks_off()
    if not left:
        ax.set_yticklabels([])
    if not bottom:
        ax.set_xticklabels([])
    for x, f in spread(runs):
        if f is CRASH:
            ax.scatter([x], [Y_CRASH], marker="x", color=MUTED, s=30, linewidths=1.4, zorder=3)
        elif f == 0:
            ax.scatter([x], [Y_ZERO], marker="o", facecolor="white", edgecolor=BLUE, s=36, linewidths=1.6,
                       zorder=4)
        else:
            ax.scatter([x], [f], marker="o", facecolor=BLUE, edgecolor="white", s=40, linewidths=1.0, zorder=4)
    ax.set_title(label, loc="left", fontsize=9.5, color=INK, pad=15)
    ax.text(0, 1.015, f"highest lossless: {best}", transform=ax.transAxes, fontsize=8.5, color=INK2,
            va="bottom")


def main(out):
    plt.rcParams.update({"font.family": ["Helvetica", "Arial", "DejaVu Sans"], "svg.fonttype": "none",
                         "font.size": 10})
    # lay out in inches, wide: groups side by side on a line while they fit in NCOL panel slots
    HEAD, TITLE, PANEL, XAX, GAP, FOOT, LEFT, RIGHT, SEP = 0.4, 0.42, 1.7, 0.3, 0.25, 0.75, 0.75, 0.15, 0.35
    blocks, cur = [], None
    for group, label, best, runs in ROWS:
        if group:
            cur = [group, []]
            blocks.append(cur)
        else:
            cur[1].append((label, best, runs))
    lines, used = [[]], 0
    for blk in blocks:
        if used and used + len(blk[1]) > NCOL:
            lines.append([])
            used = 0
        lines[-1].append(blk)
        used += len(blk[1])
    seps = max(len(line) - 1 for line in lines)
    W = 26.0
    H = len(lines) * (HEAD + TITLE + PANEL + XAX + GAP) + FOOT
    fig = plt.figure(figsize=(W, H))
    fig.patch.set_facecolor("white")
    pw = (W - LEFT - RIGHT - seps * SEP) / NCOL
    y = H  # inches from the bottom, going down
    for line in lines:
        y -= HEAD
        hy = y
        y -= TITLE + PANEL + XAX + GAP
        x = LEFT
        for group, panels in line:
            fig.text((x - 0.6) / W, (hy + 0.1) / H, group, fontsize=11, fontweight="bold", color=INK,
                     va="bottom")
            for label, best, runs in panels:
                ax = fig.add_axes([x / W, (y + XAX + GAP) / H, (pw - 0.45) / W, PANEL / H])
                panel(ax, label, best, runs, left=True, bottom=True)
                x += pw
            x += SEP
    fig.text(0.5, 0.48 / H, "Rate (Gbps): received, or sent where only that is known.   Vertical: share of "
             "events lost in the run (log).   ● one run with loss   ○ lossless run   × crashed, no result",
             ha="center", color=INK2, fontsize=9.5)
    fig.text(0.12 / W, 0.08 / H,
             "¹ Share of 2 GB files lost (a file is all or nothing). ² Receiver loss; the 3.17 Gbps run counts "
             "event numbers that never arrived (upstream).\nNot shown: 20 KB EVIO node-to-node, lossless at "
             "2.5 Gbps (the only rate run); the DAOS sink alone (--synthetic, no network), 92–186 Gbps without "
             "loss. Sources: docs/results/2026-11-12-rate-summary.md",
             color=MUTED, fontsize=8.5, va="bottom")
    fig.savefig(out, facecolor="white")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
