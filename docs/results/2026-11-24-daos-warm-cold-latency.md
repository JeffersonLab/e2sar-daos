---
type: Result
title: "DAOS commit latency, cold and warm, across every run with per-event times (analysis, 2026-11-24)"
description: "Receive → commit latency of 14 runs with per-event times (Perlmutter 2026-10-27, LUX 2026-10-28/29, node to node 2026-10-29/30; 28 receivers, 6.9M events, the old KV layout in sc26), split into cold and warm. Cold is per DAOS thread, not per run: each thread's first ~32 writes take 15–50 ms (p50), whatever the rate; p50 falls to 0.5–0.7 ms from its 33rd write and p99 to ~1 ms from its 129th. sc26 had 32 targets, and ~130 random writes are needed to touch all 32 (coupon collector), so the likely cause is a thread's first write to each target. A receiver of 32 threads therefore stays cold for ~2,000 events: 0.55–0.66 s at 40 Gbps, 4.2–4.4 s at 5 Gbps. Warm p99 is 0.86–1.15 ms, with 1–11 events over 10 ms per receiver, mostly just after warm-up. The 0.5 s time split used so far misses most cold events in slow runs. The LB 675/677/686 runs (current layout) only have whole-run percentiles; their 43–190 ms maxima fit the same start-up. New script scripts/e2sar_warm_cold_latency.py."
tags: [latency, daos, warm-up, cold-start, analysis, e2sar_daos_recv, perlmutter, lux, ebpf-testbed, sc26, lb]
sources:
  - { id: n2n, resource: "2026-10-20-e2e-n2n-sweep.md", title: "E2E node-to-node rate sweep: warm-up at the start of each run" }
  - { id: pm, resource: "2026-10-27-perlmutter-rate-sweep.md", title: "Perlmutter → LB → two receivers, 10–80 Gbps" }
  - { id: rate, resource: "2026-11-12-rate-summary.md", title: "Rates and losses by setting (LUX e2sar_perf runs)" }
  - { id: inventory, resource: "2026-11-24-data-and-logs-inventory.md", title: "Data and logs kept, as of 2026-11-24" }
  - { id: runs, resource: "ebpf testbed: ~/e2sar_run/{pm,bench,sweep,lb677,lb686,lbbeta}; laptop pm-bench/, lux-bench/, n2n-bench/", title: "Per-event CSVs and receiver logs" }
---

# DAOS commit latency, cold and warm, across every run with per-event times (analysis, 2026-11-24)

**A DAOS thread is cold for its first ~32 writes and warm from about its 65th, in every run and at every rate.** Cold writes take 15–50 ms (p50) and up to ~100 ms; warm ones take 0.5–0.7 ms (p50) and ~1 ms (p99). With 32 threads, a receiver's first ~2,000 events are cold: half a second at 40 Gbps, four seconds at 5 Gbps.

Latency here is `latency_ms` of `e2sar_daos_recv --timing-csv`: from the reassembled event handed to a DAOS (dequeue) thread until its last put completes. It doesn't include the network.

![Receive → commit latency p50 and p99 against the event's place among its DAOS thread's events, one thin line per receiver, the median over 25 receivers bold: p50 30–35 ms for the first write, 4 ms at 17–32, 0.6 ms at 33–64 and 0.5 ms after; p99 55 ms for the first writes, 19 ms at 33–64, 4 ms at 65–128 and 1.0 ms from 129 on](../images/2026-11-24-daos-warm-cold-latency.png)

*25 receivers of 13 runs (all but the two runs too short to warm up, below). x is the event's place in the order its thread received events, in powers of 2.*

## The runs

Every run whose per-event times still exist ([inventory](2026-11-24-data-and-logs-inventory.md)). All wrote to pool sc26 (32 targets) with **the old KV layout**: one `daos_kv_put` per 1 MiB chunk plus one metadata put per event.

| Run | Sender, path | Event size | Rate per receiver | Receivers × DAOS threads | Events |
|---|---|---|---|---|---|
| `pm_20261027_215642`, `_220714`, `_221223` | Perlmutter `e2sar_perf` → LB | 1 MiB | 10, 10, 40 Gbps | 2 × 32 | 10k, 100k, 100k |
| `pm_20261027_221922`, `_222315`, `_222633`, `_223531`, `_225105` ([page](2026-10-27-perlmutter-rate-sweep.md)) | Perlmutter `e2sar_perf` → LB | 1 MiB | 39, 40, 20, 10, 5 Gbps | 2 × 32 | 10⁶ each |
| `lux_20261028_161516`, `_161745`, `_161948` | LUX `e2sar_perf` → LB | 1 MiB | 26, 23, 0.05 Gbps | 2 × 32 | 100k, 100k, 1.4k |
| `lux_20261029_124858`, `_125324` | LUX `e2sar_perf` → LB | 1 MiB | 15, 15 Gbps | 2 × 32 | 385k, 987k |
| `n2n_drop_20261029_164939` | ebpf2201 `e2sar_evio_send` → ebpf2203 | 20 KB | 2.4 Gbps | 1 × 16 | 150k |
| `xm_n2n_1793327296` (2026-10-30 UTC) | `e2sar_e2e_bench.sh`, node to node | 415 B | 0.3 Gbps | 1 × 8 | 8,000 |

Two runs never warm up and are left out of the figure: `lux_20261028_161948` sent 1,448 events over 2 min (~22 per thread), and the 8,000 tiny events of `xm_n2n_1793327296` were batched into 88 array writes (~11 per thread). Their latency is cold throughout: p50 6 ms and 12 ms, p99 26–30 ms and 29 ms.

## Cold and warm, run by run

Cold = an event among its thread's first 64; warm = the rest. Ranges are over the run's receivers. "Cold until" is the receive time of the last cold event, counted from the receiver's first event.

| Run | Rate per receiver (Gbps) | Cold p99 (ms) | Cold max (ms) | Cold until (s) | Warm p50 (ms) | Warm p99 (ms) | Warm p99.9 (ms) | Warm max (ms) | Warm events > 10 ms |
|---|---|---|---|---|---|---|---|---|---|
| `pm_…221922` | 39 | 79–83 | 100–101 | 0.56–0.61 | 0.51–0.52 | 0.97–1.00 | 1.3–1.4 | 13–14 | 5, 7 |
| `pm_…222315` | 40 | 76–77 | 92–94 | 0.54–0.60 | 0.52 | 1.00–1.04 | 1.5–1.6 | 14–15 | 1, 2 |
| `pm_…221223` | 40 | 67–70 | 85–89 | 0.55–0.66 | 0.51 | 0.98–1.00 | 1.8–2.3 | 17–18 | 1, 4 |
| `lux_…161516` | 26 | 42–43 | 53–55 | 0.85–0.94 | 0.48–0.49 | 0.96–1.01 | 2.6–3.3 | 11–15 | 2, 7 |
| `lux_…161745` | 23 | 42–46 | 52–65 | 0.99–1.10 | 0.47–0.48 | 1.01–1.05 | 2.2–4.4 | 14–17 | 6, 8 |
| `pm_…222633` | 20 | 42–45 | 55–58 | 1.08–1.13 | 0.49–0.50 | 0.92–1.04 | 1.4–1.5 | 16–17 | 4, 5 |
| `lux_…124858` | 15 | 46–47 | 57–63 | 1.42–1.50 | 0.48 | 1.12–1.15 | 2.3–2.8 | 14–37 | 5, 31 |
| `lux_…125324` | 15 | 46–48 | 57–69 | 1.39–1.44 | 0.48 | 1.06 | 2.0 | 13–18 | 3, 6 |
| `pm_…220714` | 10 | 49 | 74–76 | 2.16–2.19 | 0.66–0.69 | 1.04–1.08 | 1.6–1.8 | 13–24 | 3, 6 |
| `pm_…223531` | 10 | 46–52 | 62–93 | 2.07–2.30 | 0.68 | 0.99–1.03 | 1.3–1.4 | 16–20 | 6, 11 |
| `pm_…215642` | 10 | 46–47 | 64–70 | 2.07–2.23 | 0.66–0.69 | 1.6–3.2 | 10–10.5 | 14–16 | 4, 4 |
| `pm_…225105` | 5 | 44 | 68–83 | 4.15–4.44 | 0.63–0.64 | 0.86–0.89 | 1.0–1.1 | 14–16 | 3, 6 |
| `n2n_drop_…164939` | 2.4 (20 KB) | 57 | 74 | 0.11 | 0.29 | 0.41 | 1.2 | 13 | 1 |


- **Cold doesn't depend on the rate.** The first write of a thread takes 28–50 ms (p50) in every run from 2.4 to 40 Gbps, and 17–18 ms at 0.05 Gbps (`lux_…161948`), where nothing queues behind it. The cold p99 is higher at 40 Gbps (67–83 ms) than at 5–26 Gbps (42–52 ms) because, at 40 Gbps, events arrive faster than the cold threads commit them and queue behind them.
- **Warm doesn't depend on the rate either.** p99 0.86–1.15 ms in every run of at least 50k events; p50 0.47–0.69 ms. The p50 is ~0.15 ms higher in the 5–10 Gbps Perlmutter runs than at 20–40 Gbps, as already seen in [the Perlmutter sweep](2026-10-27-perlmutter-rate-sweep.md). `pm_…215642` (10k events) has only ~90 warm events per thread, still partly in the 65–128 band, so its warm p99 is higher.
- **How long a receiver is cold is set by its event rate.** About 2,048 events (32 threads × 64) must arrive before every thread is warm, so "cold until" is roughly 64 ÷ events per second per thread, times 1.2–1.6 for the threads that get their share last: 0.55–0.66 s at 40 Gbps (~4,750 events/s), 4.2–4.4 s at 5 Gbps (~600 events/s). With 16 threads and 15,000 small events per second, `n2n_drop` was warm after 0.11 s.
- **Warm tail.** Past their thread's 64th write, 1–11 events per receiver took over 10 ms (at most 13–24 ms), on different threads. Most come in the 1–2 s after "cold until", the end of the warm-up (threads that haven't yet written to every target, below). The exception is `lux_…124858` on ebpf2201: 25 events of 10–37 ms in one 0.1 s burst at 104.7 s, on 20 threads at once, i.e. DAOS was slow for everyone for 0.1 s.

### Why the 0.5 s time split is not the right cut

[`e2sar_e2e_analyze.py --warmup 0.5`](2026-10-20-e2e-n2n-sweep.md#warm-up-at-the-start-of-each-run) and earlier results counted the first 0.5 s of a run as the warm-up. That holds at 40 Gbps (the cold phase ends at 0.55–0.66 s) but not at lower rates: at 5 Gbps only ~300 of the 2,048 cold events arrive in the first 0.5 s, and the rest are counted as "steady". In `pm_…225105`, 302 events over 10 ms fall after 0.5 s, but only 3 after each thread's 64th write. Counting per thread separates cold from warm at every rate.

![Every event of the first 6 s of two receivers, latency on a log axis, cold events blue and warm grey, with the 0.5 s split and the last cold event marked. Top, pm_20261027_222315 ebpf2201 at 40 Gbps: cold events of 10–90 ms fall to ~0.5 ms by 0.3 s, last cold event at 0.54 s. Bottom, pm_20261027_225105 ebpf2201 at 5 Gbps: cold events of 1–80 ms spread over 4 s, last cold event at 4.44 s, most of them after the 0.5 s split](../images/2026-11-24-daos-warm-cold-latency_time.png)

*At 5 Gbps each thread gets its 64 events only over ~4 s, so slow writes keep appearing long after 0.5 s. After warm-up the latency sits in two bands, ~0.55 and ~0.75 ms; what separates them hasn't been checked.*

## Why: a thread's first write to each target

sc26 had **32 targets**, and each run started a new container. A thread's chunk keys hash to targets at random, and a thread touches all 32 targets only after about 32 × (1 + 1/2 + … + 1/32) ≈ 130 writes on average (the coupon collector). That matches the curve: nearly every write is slow until the thread has written ~16–32 times, p50 is warm by its 33rd write, while p99 stays at 4–7 ms up to its 128th and reaches ~1 ms only after that. This is what a fixed cost for each thread's first write to each target would give: opening the connection or the object shard on that target, the first registration of a buffer, or both.

Not proven. Things that don't fit a per-run cost (opening the pool or the container once): it repeats on every thread, and in `lux_…161948` (~12 events/s) a thread's first write is still 17–18 ms when it comes seconds into the run. The in-flight depth doesn't decide it either: 256 puts per thread in the Perlmutter and LUX runs, 128 in `n2n_drop`, and the curve is the same.

## Runs without per-event times

The LB 675, 677 and 686 runs and the 63 tuning runs of 2026-10-29 wrote with the current layout (events batched into array writes, flush after 5 ms) and kept only the receiver's end-of-run percentiles, which are log-binned (4.194 ms = 2²² ns). They can't be split into cold and warm. What they show:

| Run | Event size | Rate per receiver (Gbps) | Event p50 (ms) | Event p99 (ms) | Event max (ms) | Array write p99 (ms) | Array write max (ms) |
|---|---|---|---|---|---|---|---|
| `e2sar_lb675_1793817098` (2 receivers) | 207 B | 0.013 | 4.19 | 6.29 | 61–67 | 0.10 | 38–39 |
| `e2sar_lb677_1793905825` run 1 (2) | 9.5 KB | 0.58 | 4.19 | 6.82 | 58–60 | 0.21–0.25 | 40–43 |
| `e2sar_lb677_1793909535` run 2 (2) | 9.5 KB | 0.63–0.88 | 3.93–4.19 | 6.82 | 49–67 | 0.23–0.66 | 27–32 |
| `e2sar_lb686_1794441647`, `_1794444385` (1 each) | 26 KB | 3.1–3.2 | 3.67 | 6.29 | 52–55 | 0.39 | 37–38 |
| `e2sar_lb686_1794459212` (2) | 171 KB | 10.5 | 4.19 | 6.82 | 43–52 | 0.59–0.66 | 33–43 |
| `e2sar_lb686_1794515182` (2) | 381 KB | 21.2–21.3 | 5.24 | 7.86–8.39 | 87–96 | 1.8–2.6 | 53–59 |
| `e2sar_lb686_1794515844` (2) | 381 KB | 20.4–20.5 | 3.15 | 7.34 | 181–190 | 1.8–2.0 | 159–178 |

- **The event p50 of ~4 ms is the batch flush, not DAOS.** Small events wait up to 5 ms for their batch to fill. One array write takes 0.1–0.7 ms (p99) up to 10 Gbps and ~2 ms at 20 Gbps.
- **The maxima fit the same cold start.** A single array write's maximum is 27–59 ms in every run but the last, the same as a cold put in the runs above, while its p99 is under 2.6 ms. Without per-event times the time of these maxima isn't known.
- **`e2sar_lb686_1794515844` is the exception:** array writes of up to 159–178 ms on both nodes, three times any other run. That run filled sc-demo to 82% in 33 min; whether the slow writes came at the start or as the pool filled is open.
- The tuning sweep: median over its 63 runs of a single array write's maximum 33 ms (12–1,361 ms; the largest in overloaded settings).

## Next steps

- [ ] **Warm up each thread before registering with the LB:** in `e2sar_daos_recv`, have every DAOS thread write (and remove) one value on each target of the pool before the receiver reports ready, then repeat a 5 Gbps and a 40 Gbps run. If the hypothesis holds, the first-write curve above goes flat, and so should the start-of-run losses of [the node-to-node sweep](2026-10-20-e2e-n2n-sweep.md#the-lost-events) and [LB 682](2026-11-10-lb682-rate-sweep-ramp.md).
- [ ] **Per-event times in LB runs:** run `e2sar_daos_recv --timing-csv` in the live pipeline (it costs one line per event; ~26M lines for a 10 TB run) or log per-thread array-write latency for the first 256 writes only.
- [ ] **The current layout:** repeat the split with `--timing-csv` on sc-demo (also 32 targets) to check that a thread's first ~130 array writes behave as its first ~130 KV puts did.
- [ ] **When the slow writes of `e2sar_lb686_1794515844` happened:** at the start, or as sc-demo filled?

## Reproducing

`scripts/e2sar_warm_cold_latency.py` ([scripts](../reference/scripts.md)) prints both tables and draws the two figures, from run directories with one `--timing-csv` file per receiver:

```
cd ~/Documents/DAQ        # laptop; pm_215642/220714/221223 copied from ebpf2201 ~/e2sar_run/pm/
e2sar-daos/scripts/e2sar_warm_cold_latency.py \
    --plots e2sar-daos/docs/images/2026-11-24-daos-warm-cold-latency \
    --time pm_20261027_222315/ebpf2201,pm_20261027_225105/ebpf2201 \
    pm-bench/pm_20261027_* lux-bench/lux_20261028_161516 lux-bench/lux_20261028_161745 \
    lux-bench/lux_20261029_* n2n-bench/n2n_drop_20261029_164939
```

`--cold-writes N` moves the cold/warm cut (default 64), `--csv OUT` writes the run-by-run table, `--time` picks the two receivers of the time view. The tables need only the Python standard library; the figures need matplotlib. The end-of-run percentiles above are the `Event latency (recv->commit)` and `daos_array_write latency` lines at the end of each `recv*.log` under `~/e2sar_run`.
