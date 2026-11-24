#!/usr/bin/env python3
"""Arrival statistics of a run from the receivers' --timing-csv files.

Usage: e2sar_arrival_stats.py RUN_DIR [RUN_DIR ...]

Each RUN_DIR holds <host>.csv (e2sar_daos_recv --timing-csv) and <host>.recv.log
for every receiver, as in ~/e2sar_run/pm/pm_* and the laptop's lux-bench/lux_*.
Prints one line per run and receiver:

  R          mean rate (Gbps) over the receive window
  pk1ms      peak and p99 of the rate in 1 ms bins (Gbps): how bursty the arrivals are
  idle1ms    share of 1 ms bins with no event completed (on/off sending)
  gapmax     longest gap between two completed events (ms)
  lag p50/p90  how far (ms) each event completes from its place in event-number order,
             on the same receiver (no cross-host clocks): the time over which an event's
             packets arrive, i.e. how long one event is "in flight"
  K          events in flight on this receiver = rate (events/s) x lag p90 (Little's law)
  r1         rate of one in-flight event = R / K (Gbps)

Standard library only.
"""
import csv
import glob
import os
import sys
from collections import Counter


def load(path):
    with open(path) as fh:
        return sorted((float(r['recv_s']), int(r['evt_num']), int(r['bytes'])) for r in csv.DictReader(fh))


def stats(ev):
    size = ev[0][2]
    t0, t1 = ev[0][0], ev[-1][0]
    window = t1 - t0
    lam = len(ev) / window
    rate = 8 * size * lam / 1e9
    bins = Counter(int((t - t0) / 0.001) for t, _, _ in ev)
    nb = int(window / 0.001) + 1
    counts = sorted(bins.get(i, 0) for i in range(nb))
    to_gbps = lambda n: 8 * size * n / 0.001 / 1e9
    gaps = [b[0] - a[0] for a, b in zip(ev, ev[1:])]
    order = [t for t, _, _ in ev]
    rank = {n: i for i, (_, n, _) in enumerate(sorted(ev, key=lambda e: e[1]))}
    lag = sorted(abs(t - order[rank[n]]) for t, n, _ in ev)
    q = lambda p: lag[int(p * (len(lag) - 1))]
    k = lam * q(0.9)
    return dict(n=len(ev), size=size, R=rate, pk=to_gbps(counts[-1]), p99=to_gbps(counts[int(0.99 * (nb - 1))]),
                idle=100 * counts.count(0) / nb, gapmax=1e3 * max(gaps), lag50=1e3 * q(0.5), lag90=1e3 * q(0.9),
                K=k, r1=rate / k if k else float('nan'))


def main(dirs):
    print(f"{'run':24}{'host':10}{'events':>9}{'R':>7}{'pk1ms':>7}{'p99':>6}{'idle%':>7}{'gapmax':>8}"
          f"{'lag50':>7}{'lag90':>7}{'K':>6}{'r1':>6}")
    for d in dirs:
        for f in sorted(glob.glob(os.path.join(d, '*.csv'))):
            host = os.path.basename(f)[:-4]
            ev = load(f)
            if len(ev) < 1000:
                continue
            s = stats(ev)
            print(f"{os.path.basename(os.path.normpath(d)):24}{host:10}{s['n']:9}{s['R']:7.1f}{s['pk']:7.1f}{s['p99']:6.1f}"
                  f"{s['idle']:7.1f}{s['gapmax']:8.1f}{s['lag50']:7.2f}{s['lag90']:7.2f}{s['K']:6.1f}{s['r1']:6.1f}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
