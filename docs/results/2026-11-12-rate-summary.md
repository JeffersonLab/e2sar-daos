---
type: Result
title: "Rates and losses by setting, 2026-10-19 to 2026-11-12"
description: "One chart and table of every measured rate and its event loss, from e2sar_perf alone, node-to-node and through the LB into DAOS, up to the LB 686 40.8 Gbps run: the highest lossless rate per setting, the highest rate reached, and what changed it."
tags: [summary, rate-sweep, loss, e2sar_perf, lb, node-to-node]
sources:
  - { id: perf, resource: "2026-10-26-e2sar-perf-n2n-vs-lb.md", title: "e2sar_perf without DAOS, node-to-node vs LB" }
  - { id: bench, resource: "2026-10-19-e2e-bench.md", title: "E2E bench, 2026-10-19" }
  - { id: n2n, resource: "2026-10-20-e2e-n2n-sweep.md", title: "E2E node-to-node rate sweep" }
  - { id: lbsweep, resource: "2026-10-21-e2e-lb-sweep.md", title: "E2E LB rate sweep" }
  - { id: ft, resource: "2026-10-19-lb-e2sar-ft.md", title: "Through the ESnet LB with e2sar_ft" }
  - { id: pm, resource: "2026-10-27-perlmutter-rate-sweep.md", title: "Perlmutter rate sweep" }
  - { id: tune, resource: "2026-10-29-receiver-tuning.md", title: "Receiver tuning" }
  - { id: lb682, resource: "2026-11-10-lb682-rate-sweep-ramp.md", title: "LB 682 rate sweep with and without a ramp" }
  - { id: lb686, resource: "2026-11-12-203725UTC-lb686-40gbps-10tb.md", title: "LB 686 at 40.8 Gbps" }
---

# Rates and losses by setting, 2026-10-19 to 2026-11-12

Every rate measured so far and how many events each run lost, one row per setting. [e2e-summary](e2e-summary.md) covers the runs up to 2026-10-28 in more detail; this page adds the runs since then, up to the LB 686 run at 40.8 Gbps on 2026-11-12.

![One panel per setting: rate in Gbps across, share of events lost on a log scale up, lossless runs and crashes in strips below. Lossless runs reach 20 Gbps with e2sar_perf node-to-node, 35 Gbps through LB 638, 40 Gbps with 1 MiB events from 8 streams of 2 sockets, and 15 Gbps on LB 682 with a sender ramp; every run from 45 Gbps up lost 10⁻⁵ to 10⁻³ of its events, except a few runs of 1–2.4% and the 2 GB files, lost whole](../images/e2sar_rate_summary.svg)

One panel per setting and one marker per run: the rate it reached across, the share of its events lost up (log scale), with lossless runs (○) and crashed runs (×) in the strips below. The chart is drawn by [`scripts/e2sar_rate_summary.py`](../../scripts/e2sar_rate_summary.py), with the numbers typed in from the pages below. To add a run, add it there and draw the chart again.

## Table

*Lossless* is the highest rate at which a run lost no events, and how many runs at that rate were lossless. *Highest reached* is the fastest run and its loss rate.

| Setting | Path | Lossless | Highest reached | Source |
|---|---|---|---|---|
| **E2SAR alone** (`e2sar_perf` at both ends, no DAOS), 1 MiB events, 16 sockets → 32 threads, `--smooth` | | | | |
| plain | node-to-node | 20 Gbps (1 of 2; the other crashed) | 87 Gbps sent, 5×10⁻⁴ | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| `-o recvmmsg --rcviovecsize 1024` | node-to-node | 20 Gbps (1 of 2; the other crashed) | 88 Gbps sent, 5×10⁻⁴ and 1.4% | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| plain | LB 86 | 10 Gbps (2 of 2) | 89.6 Gbps, 3×10⁻⁴ | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| recvmmsg 1024 | LB 86 | 10 Gbps (2 of 2); 30 Gbps (1 of 2) | 89.7 Gbps, 4–6×10⁻⁴ | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| **Node-to-node into DAOS**, ebpf2201 → `e2sar_daos_recv` on ebpf2203 | | | | |
| 95 KB events, 16 Segmenters, 16 dequeue threads | node-to-node | 25 Gbps (1 of 2) | 35.3 Gbps, 5–14×10⁻⁵ | [bench §4](2026-10-19-e2e-bench.md#4-highest-lossless-rate) |
| 95 KB, rate sweep, 6 runs per rate | node-to-node | 30 Gbps (1 of 6); 25 Gbps (2 of 6) | 50.9 Gbps, 0.7–3×10⁻⁴ | [n2n §7](2026-10-20-e2e-n2n-sweep.md) |
| 1 MiB, 8 streams × 8 sockets, `--deq 8` | node-to-node | none (20 Gbps lost 3) | 79.8 Gbps, 2.4×10⁻⁴ | [tuning c2](2026-10-29-receiver-tuning.md) |
| **1 MiB, 8 streams × 2 sockets, `--deq 8 --inflight 32`** (recommended) | node-to-node | **40 Gbps** (1 of 1) | 80 Gbps, 1.0×10⁻⁴ | [tuning c3](2026-10-29-receiver-tuning.md) |
| 20 KB EVIO, 1–4 streams | node-to-node | 2.5 Gbps (the only rate run) | 2.5 Gbps | [tuning a](2026-10-29-receiver-tuning.md), [evio-20kb](2026-10-30-evio-20kb-2p5gbps.md) |
| **Through the LB into DAOS** | | | | |
| 95 KB, 16 Segmenters | LB 638 | **35 Gbps** (2 of 2); 20 Gbps (6 of 6) | 45.5 Gbps, 5–7×10⁻⁵ | [bench §4–5](2026-10-19-e2e-bench.md#4-highest-lossless-rate) |
| 95 KB, rate sweep | LB 640 | 25 Gbps (1 of 2); 20 Gbps (1 of 1) | 45.6 Gbps, 4–9×10⁻⁵ | [LB §9](2026-10-21-e2e-lb-sweep.md) |
| 2 GB files with `e2sar_ft` | LB 638 | 25.8 Gbps (2 of 4); 11.5 Gbps (3 of 3) | 34.4 Gbps, 2–4 of 4 files lost | [e2sar_ft](2026-10-19-lb-e2sar-ft.md) |
| 1 MiB from one Perlmutter node, 2 receivers | ESnet LB | none (10 Gbps lost 1.3×10⁻⁴) | 79.8 Gbps, 9.7×10⁻⁵ | [Perlmutter](2026-10-27-perlmutter-rate-sweep.md) |
| ~100 KB EVIO, 1 receiver | LB 682 | 10 Gbps | 20 Gbps, 4.6×10⁻⁵ | [LB 682](2026-11-10-lb682-rate-sweep-ramp.md) |
| same, `e2sar_evio_send --ramp 10` | LB 682 | **15 Gbps** | 20 Gbps, 1.4×10⁻⁶ (3 events) | [LB 682](2026-11-10-lb682-rate-sweep-ramp.md) |
| External senders, 2 receivers, 16–32 receive threads each | LB 686 | none | 40.8 Gbps for 33 min, 1.09×10⁻³; 20.9 Gbps for 64 min, 2.75×10⁻⁵ | [20.9](2026-11-12-045334UTC-lb686-two-workers-10tb.md), [40.8](2026-11-12-203725UTC-lb686-40gbps-10tb.md) |
| **DAOS alone** (`--synthetic`, no network) | none | 92–103 Gbps (95 KB), 170–186 Gbps (1 MiB, 1.5 MB) | | [synthetic](2026-10-20-synthetic-sink.md), [tuning c1](2026-10-29-receiver-tuning.md) |

Loss rates are events lost over events sent, except:
- the `e2sar_ft` row counts 2 GB files, and one lost packet loses a whole file;
- the LB 686 rows count the receivers' reassembly loss, since the senders aren't ours. The 3.17 Gbps run in the chart ([live-rm](2026-11-12-004630UTC-lb686-live-rm.md)) counts the 0.355% of event numbers that never arrived; its receiver lost 1 event.

The low-rate EVIO streams from NERSC (LB 675 at 25 Mbit/s, LB 677 at 1.2 Gbps) lost 0.003–0.03%, all upstream or in bursts that no receiver counter saw. They aren't in the chart.

## What the chart shows

1. **The highest lossless rate is 35–40 Gbps, and only sometimes.** It was reached by 95 KB events through LB 638 (2 of 2 runs) and by 1 MiB events node-to-node from 8 streams of 2 sockets (1 run). Every run at 45 Gbps or more lost events. Lossless in every run: 20 Gbps through the LB.
2. **The highest rate reached is ~90 Gbps for E2SAR alone and ~80 Gbps into DAOS.** E2SAR alone lost 3–6×10⁻⁴ of the events at ~90 Gbps. Into DAOS, the runs at ~80 Gbps lost 2×10⁻⁵ to 2×10⁻⁴. DAOS itself commits 92–186 Gbps, so it isn't what limits the rate.
3. **The loss rate depends on the setting more than on the rate.** It was 10⁻⁵–10⁻⁴ at 80 Gbps from Perlmutter, but 0.49% at 40 Gbps in the same session. LB 686 lost 40× more at 40.8 Gbps than at 20.9.
4. **Changes that helped:**
   - **Fewer sender sockets per stream:** 2 instead of 8 halved the loss at 80 Gbps and made 40 Gbps lossless.
   - **A ramp at the start:** with `--ramp 10`, the lossless rate went from 10 to 15 Gbps, and 20 Gbps lost 3 events instead of 101.
   - **The LB path:** it smooths the bursts into DAOS (35 Gbps lossless vs 25–30 node-to-node). With `e2sar_perf` alone it didn't: 10 vs 20 Gbps.
5. **Changes that didn't help:**
   - **recvmmsg:** with it, 3 runs node-to-node lost 1.4–2.4%, at 40, 80 and 90 Gbps. 2 runs crashed.
   - **More dequeue or reassembly threads than streams:** they cost CPU and didn't lose less.
   - **Pinning the receive threads to NUMA 0:** the NIC port discards got ~10× worse.
6. **Nearly every lost event came with NIC port discards** (`rx_discards_phy`), plus upstream loss on the LB 686 runs. DAOS never failed a write or fell behind. See [lossless-ceiling](../concepts/lossless-ceiling.md).
