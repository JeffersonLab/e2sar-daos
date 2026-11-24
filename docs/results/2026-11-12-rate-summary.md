---
type: Result
title: "Rates and losses by setting, 2026-10-19 to 2026-11-12"
description: "One chart and table of every measured rate and its event loss, from e2sar_perf alone, node-to-node and through the LB into DAOS, up to the LB 686 40.8 Gbps run: the highest lossless rate per setting, the highest rate reached, and what changed it."
tags: [summary, rate-sweep, loss, e2sar_perf, lb, node-to-node, perlmutter, nersc, lux, olcf]
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

| Date | Kind | Setting | Path | Lossless | Highest reached | Source |
|---|---|---|---|---|---|---|
| 2026-10-19 | into DAOS | 95 KB events, 16 Segmenters, 16 dequeue threads | node-to-node | 25 Gbps (1 of 2) | 35.3 Gbps, 5–14×10⁻⁵ | [bench §4](2026-10-19-e2e-bench.md#4-highest-lossless-rate) |
| 2026-10-19 | into DAOS | 95 KB, 16 Segmenters | LB 638 | **35 Gbps** (2 of 2); 20 Gbps (6 of 6) | 45.5 Gbps, 5–7×10⁻⁵ | [bench §4–5](2026-10-19-e2e-bench.md#4-highest-lossless-rate) |
| 2026-10-19 | into DAOS | 2 GB files with `e2sar_ft` | LB 638 | 25.8 Gbps (2 of 4); 11.5 Gbps (3 of 3) | 34.4 Gbps, 2–4 of 4 files lost | [e2sar_ft](2026-10-19-lb-e2sar-ft.md) |
| 2026-10-20 | into DAOS | 95 KB, rate sweep, 6 runs per rate | node-to-node | 30 Gbps (1 of 6); 25 Gbps (2 of 6) | 50.9 Gbps, 0.7–3×10⁻⁴ | [n2n §7](2026-10-20-e2e-n2n-sweep.md) |
| 2026-10-20, 10-29 | DAOS alone | `--synthetic`, no network | none | 92–103 Gbps (95 KB), 170–186 Gbps (1 MiB, 1.5 MB) | | [synthetic](2026-10-20-synthetic-sink.md), [tuning c1](2026-10-29-receiver-tuning.md) |
| 2026-10-21 | into DAOS | 95 KB, rate sweep | LB 640 | 25 Gbps (1 of 2); 20 Gbps (1 of 1) | 45.6 Gbps, 4–9×10⁻⁵ | [LB §9](2026-10-21-e2e-lb-sweep.md) |
| 2026-10-26 | E2SAR alone | `e2sar_perf` at both ends, 1 MiB, 16 sockets → 32 threads, `--smooth`; plain | node-to-node | 20 Gbps (1 of 2; the other crashed) | 87 Gbps sent, 5×10⁻⁴ | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| 2026-10-26 | E2SAR alone | same, `-o recvmmsg --rcviovecsize 1024` | node-to-node | 20 Gbps (1 of 2; the other crashed) | 88 Gbps sent, 5×10⁻⁴ and 1.4% | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| 2026-10-27 | into DAOS | 1 MiB from one Perlmutter node, 2 receivers | ESnet LB | none (10 Gbps lost 1.3×10⁻⁴) | 79.8 Gbps, 9.7×10⁻⁵ | [Perlmutter](2026-10-27-perlmutter-rate-sweep.md) |
| 2026-10-28 | E2SAR alone | same as 10-26, plain | LB 86 | 10 Gbps (2 of 2) | 89.6 Gbps, 3×10⁻⁴ | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| 2026-10-28 | E2SAR alone | same, recvmmsg 1024 | LB 86 | 10 Gbps (2 of 2); 30 Gbps (1 of 2) | 89.7 Gbps, 4–6×10⁻⁴ | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| 2026-10-28, 10-29 | into DAOS | 1 MiB from one LUX node, 2 receivers | ESnet LB | none | 51.4 Gbps, 5.6×10⁻³ | [below](#every-run-sender-mtu-configuration-and-rate) |
| 2026-10-29 | into DAOS | 1 MiB, 8 streams × 8 sockets, `--deq 8` | node-to-node | none (20 Gbps lost 3) | 79.8 Gbps, 2.4×10⁻⁴ | [tuning c2](2026-10-29-receiver-tuning.md) |
| 2026-10-29 | into DAOS | **1 MiB, 8 streams × 2 sockets, `--deq 8 --inflight 32`** (recommended) | node-to-node | **40 Gbps** (1 of 1) | 80 Gbps, 1.0×10⁻⁴ | [tuning c3](2026-10-29-receiver-tuning.md) |
| 2026-10-29, 10-30 | into DAOS | 20 KB EVIO, 1–4 streams | node-to-node | 2.5 Gbps (the only rate run) | 2.5 Gbps | [tuning a](2026-10-29-receiver-tuning.md), [evio-20kb](2026-10-30-evio-20kb-2p5gbps.md) |
| 2026-11-10 | into DAOS | ~100 KB EVIO, 1 receiver | LB 682 | 10 Gbps | 20 Gbps, 4.6×10⁻⁵ | [LB 682](2026-11-10-lb682-rate-sweep-ramp.md) |
| 2026-11-10 | into DAOS | same, `e2sar_evio_send --ramp 10` | LB 682 | **15 Gbps** | 20 Gbps, 1.4×10⁻⁶ (3 events) | [LB 682](2026-11-10-lb682-rate-sweep-ramp.md) |
| 2026-11-12 | into DAOS | External senders, 2 receivers, 16–32 receive threads each | LB 686 | none | 40.8 Gbps for 33 min, 1.09×10⁻³; 20.9 Gbps for 64 min, 2.75×10⁻⁵ | [20.9](2026-11-12-045334UTC-lb686-two-workers-10tb.md), [40.8](2026-11-12-203725UTC-lb686-40gbps-10tb.md) |

Loss rates are events lost over events sent, except:
- the `e2sar_ft` row counts 2 GB files, and one lost packet loses a whole file;
- the LB 686 rows count the receivers' reassembly loss, since the senders aren't ours. The 3.17 Gbps run in the chart ([live-rm](2026-11-12-004630UTC-lb686-live-rm.md)) counts the 0.355% of event numbers that never arrived; its receiver lost 1 event.

The low-rate EVIO streams from NERSC (LB 675 at 25 Mbit/s, LB 677 at 1.2 Gbps) lost 0.003–0.03%, all upstream or in bursts that no receiver counter saw. They aren't in the chart.

## Every run: sender, MTU, configuration and rate

One row per run or set of runs, in date order, with the MTU the sender used. *Rate* is the rate into the receivers (both together where there are two): lossless means the highest rate with 0 events lost. *Loss* is events lost over events sent, or the receivers' reassembly loss where the senders aren't ours. *MTU* comes from the sender's command or log, or from the data per packet at the receivers (at most 1,436 B of event data at MTU 1500, 8,936 B at 9000).

| Date | Runs | Sender | Path | Receivers | Event size | MTU | Rate | Loss | Source |
|---|---|---|---|---|---|---|---|---|---|
| 2026-10-19 | MTU test | ebpf2201, `e2sar_ft --rate 5` | node-to-node | `e2sar_daos_recv`, ebpf2203 | 2 GB files | **1500 and 9000** | 2.1 Gbps at 1500, 7.3–11.5 Gbps at 9000 | | [bottleneck](2026-10-19-bottleneck-e2sar-ft.md) |
| 2026-10-19 | e2e bench | ebpf2201, `e2sar_evio_send`, 16 Segmenters | node-to-node; LB 638 | ebpf2203, 16 dequeue threads | 95 KB | 9000 | lossless 25 Gbps (node-to-node), 35 Gbps (LB 638); highest 35.3 and 45.5 Gbps | 5–14×10⁻⁵, 5–7×10⁻⁵ | [bench](2026-10-19-e2e-bench.md) |
| 2026-10-19 | `e2sar_ft` sweep | ebpf2201, `e2sar_ft` | LB 638 | ebpf2203 | 2 GB files | 9000 | lossless 25.8 Gbps (2 of 4); highest 34.4 Gbps | 2–4 of 4 files | [e2sar_ft](2026-10-19-lb-e2sar-ft.md) |
| 2026-10-20 | 54 runs, 20–50 Gbps | ebpf2201, `e2sar_evio_send` | node-to-node | ebpf2203 | 95 KB | 9000 | lossless 30 Gbps (1 of 6); highest 50.9 Gbps | 0.7–3×10⁻⁴ | [n2n](2026-10-20-e2e-n2n-sweep.md) |
| 2026-10-20, 10-29 | `--synthetic`, no network | none | none | `e2sar_daos_recv` | 95 KB – 1.5 MB | – | 92–186 Gbps into DAOS | | [synthetic](2026-10-20-synthetic-sink.md) |
| 2026-10-21 | 11 runs, 20–45 Gbps | ebpf2201, `e2sar_evio_send` | LB 640 | ebpf2203 | 95 KB | 9000 | lossless 25 Gbps; highest 45.6 Gbps | 4–9×10⁻⁵ | [LB](2026-10-21-e2e-lb-sweep.md) |
| 2026-10-26, 10-28 | 59 runs, 10–90 Gbps, no DAOS | ebpf2201, `e2sar_perf`, 16 sockets, `--smooth` | node-to-node; LB 86 | `e2sar_perf -r`, ebpf2203, 32 threads | 1 MiB | 9000 | lossless 20 Gbps (node-to-node), 10–30 Gbps (LB 86); highest 87–88 and 89.7 Gbps | 3–6×10⁻⁴ (1.4–2.4% in 3 recvmmsg runs) | [perf](2026-10-26-e2sar-perf-n2n-vs-lb.md) |
| 2026-10-27 | 8 runs, 5–80 Gbps | **Perlmutter**, 1 CPU node, `e2sar_perf` | ESnet LB | ebpf2201 + ebpf2203 | 1 MiB | 9000 | none lossless; highest 79.8 Gbps | 1.7–9.7×10⁻⁵ at ~80 Gbps, 0.49% at 40 | [Perlmutter](2026-10-27-perlmutter-rate-sweep.md) |
| 2026-10-28, 10-29 | 5 runs, not written up | **LUX**, 1 node, `e2sar_perf` | ESnet LB | ebpf2201 + ebpf2203, `sc26/e2sar-lux`, 32 dequeue threads | 1 MiB | 9000 (8,610–8,838 B per packet) | none lossless; 30.3, 30.6, 45.9 and 51.4 Gbps | 5.6×10⁻³–1.3×10⁻² | laptop `~/Documents/DAQ/lux-bench` |
| 2026-10-29 | receiver tuning | ebpf2201, `e2sar_perf`, 8 streams × 2 sockets | node-to-node | ebpf2203, `--deq 8 --inflight 32` | 1 MiB | 9000 | lossless 40 Gbps; highest 80 Gbps | 1.0×10⁻⁴ | [tuning](2026-10-29-receiver-tuning.md) |
| 2026-10-30 | 1 run | ebpf2201, `e2sar_evio_send`, 4 streams | node-to-node | ebpf2203 + `--live-rm` reader | 20 KB | 9000 | lossless 2.5 Gbps | 0 | [evio-20kb](2026-10-30-evio-20kb-2p5gbps.md) |
| 2026-11-04 | LB 675, 51 min | **NERSC** frame builder (not ours) | LB 675 | ebpf2201 + ebpf2203 + `--live` | 207 B | one packet per event | 25 Mbit/s | 3.6×10⁻⁵ | [LB 675](2026-11-04-nersc-lb675-evio.md) |
| 2026-11-05 | LB 677 runs 1–2 | **NERSC** and **LUX** frame builders (not ours) | LB 677 | ebpf2201 + ebpf2203 + `--live` | 9.5–10 KB | 1500 (~10,700 packets for 1,399 events) | 1.16–1.23 Gbps | 3×10⁻⁵, 2.8×10⁻⁴ | [run 1](2026-11-05-194730UTC-lb677-run1-evio.md), [run 2](2026-11-05-201815UTC-lb677-run2-evio.md) |
| 2026-11-09 | LB 682, 11 runs | **Perlmutter** and **LUX** frame builders (not ours) | LB 682 | ebpf2201 | 28–45 KB | 1500 (1,498-byte frames) | 3.4–3.8 Gbps | ~10% with 3 MB socket buffers; crashes from event number collisions | [LB 682](2026-11-09-lb682-perlmutter-losses.md) |
| 2026-11-10 | LB 682 sweep, 8 runs | ebpf2203, `e2sar_evio_send`, 4 streams × 4 sockets | LB 682 | ebpf2201, 4 threads | ~100 KB | 9000 | lossless 10 Gbps, 15 with `--ramp 10`; highest 20 Gbps | 4.6×10⁻⁵, 1.4×10⁻⁶ with the ramp | [LB 682 sweep](2026-11-10-lb682-rate-sweep-ramp.md) |
| 2026-11-12 | LB 686 1794444385 | **LUX** and JLab 129.57.177.19 (not ours) | LB 686 | ebpf2201 + `--live-rm` reader on ebpf2203 | 26 KB | 1500 (1,398 B per packet) | 3.17 Gbps | 3×10⁻⁸ (1 event); 0.355% event numbers never arrived | [live-rm](2026-11-12-004630UTC-lb686-live-rm.md) |
| 2026-11-12 | LB 686 1794459212, 10 TB | **LUX** and JLab (not ours) | LB 686 | ebpf2201 + ebpf2203, 16 threads each | 171 KB | 1500 (1,430 B) | 20.9 Gbps, 64 min | 2.75×10⁻⁵ | [20.9 Gbps](2026-11-12-045334UTC-lb686-two-workers-10tb.md) |
| 2026-11-12 | LB 686 1794515182 | **LUX** and JLab (not ours) | LB 686 | ebpf2201 + ebpf2203, 32 threads, `--deq 32` | 387 KB | 1500 (1,433 B) | 45 Gbps, 7 min | 6.1×10⁻⁴ | [40.8 Gbps](2026-11-12-203725UTC-lb686-40gbps-10tb.md) |
| 2026-11-12 | LB 686 1794515844, 10 TB (kept) | **LUX** and JLab (not ours) | LB 686 | ebpf2201 + ebpf2203, 32 threads, `--deq 16` | 381 KB | 1500 (1,432 B) | 40.8 Gbps, 33 min | 1.09×10⁻³ | [40.8 Gbps](2026-11-12-203725UTC-lb686-40gbps-10tb.md) |

- **Every sender of ours used MTU 9000**, from JLab, Perlmutter and LUX. Every run at 1500 had a frame builder that isn't ours as sender: LB 677, LB 682 and LB 686. The path carries 9000 from LUX and from Perlmutter ([LUX path](2026-11-23-lux-lb-path.md)), so those senders could send ~6× fewer packets for the same rate.
- The 2026-10-28/29 LUX runs aren't in the chart: they're in the laptop folder only (receiver logs, NIC counters, `lux_loss_2026-10-28_29.png`). Into the `sc26` pool, they lost 0.6–1.3% of the events at 30–51 Gbps, 10–100× more than Perlmutter at 80 Gbps the day before.

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
