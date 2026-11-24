---
type: How-To
title: "The upper limit of LCF → LB → JLab streaming, from latency, RTT and buffer measurements"
description: "How to compute the highest rate Perlmutter or LUX → ESnet LB → e2sar_daos_recv at JLab can carry: Little's law (rate = in flight / latency) per stage, why the WAN RTT doesn't cap UDP streaming but does cap TCP (BDP, Mathis), how stalls and buffers set the lossless rate (rate × stall ≤ buffer), packet and event rate ceilings, and a worked table with the measured numbers: the receivers' NIC port, not the WAN, is the limit today (~25–35 Gbps lossless per receiver node)."
tags: [throughput, latency, rtt, bdp, mtu, littles-law, perlmutter, lux, lb, ejfat, analysis]
sources:
  - { id: path, resource: "../results/2026-11-23-lux-lb-path.md", title: "The network path LUX → ESnet LB → JLab" }
  - { id: metrics, resource: "../concepts/throughput-metrics.md", title: "Throughput and latency metrics" }
  - { id: ceiling, resource: "../concepts/lossless-ceiling.md", title: "The lossless ceiling and the port discards" }
  - { id: summary, resource: "../results/2026-11-12-rate-summary.md", title: "Rates and losses by setting" }
---

# The upper limit of LCF → LB → JLab streaming, from latency, RTT and buffer measurements

The goal: given measured latencies (WAN RTT, DAOS commit time, per-event processing time) and buffer sizes, compute the highest rate that a Perlmutter or LUX sender can stream through the ESnet LB into DAOS at JLab, and find which stage sets it.

```
LCF node ──WAN──▶ ESnet LB ──WAN──▶ JLab NIC port ─▶ RX ring ─▶ socket ─▶ Reassembler ─▶ dequeue threads ─▶ DAOS
 (sender)        (one-way ~30 ms)   (port buffer)   (8192 pkts) (1 GiB)  (~85k ev/s)    (--deq × --inflight)
```

## 1. The one relation: Little's law

For any stage that holds work while it is being processed:

> **in flight = rate × latency**, so **max rate = max in flight / latency**

In events: `λ_max = N / T`, where `N` is how many events the stage can hold at once and `T` the time each spends in it. In Gbps: `R_max = 8 · S · N / T` for events of `S` bytes. The streaming chain's upper limit is the **smallest `R_max` over all stages**, together with the line rates (100 Gb/s at the ebpf NICs, 200 Gb/s on Perlmutter's `hsn0`).

Start from the event rate, because most stages are limited per event, not per byte:

```
λ = R / (8 · S)        e.g. 40 Gbps of 381 KB events = 13.1k events/s
```

## 2. The WAN RTT doesn't cap UDP streaming

E2SAR sends UDP with no acknowledgements and no window, so the sender never waits for the far end. The data on the wire is `R × one-way delay`: 40 Gbps × 66 ms (LUX → LB → JLab) = 330 MB in flight. The network holds it and nobody waits for it. A longer RTT adds **latency** (an event takes ~66–68 ms LUX → JLab, ~75 ms ebpf → LB → ebpf; see [the E2E LB sweep](../results/2026-10-21-e2e-lb-sweep.md)), not a lower rate.

The RTT **does** cap anything that waits for an answer:

| What | Limit | LUX → LB (60.5 ms) | JLab → LB (75.6 ms) |
|---|---|---|---|
| TCP, one stream, window W | `R ≤ W / RTT` (BDP: W ≥ R × RTT) | 302 MB window for 40 Gbps | 378 MB |
| TCP, one stream, loss p (Mathis) | `R ≲ (MSS / RTT) · 1.22 / √p` | 1.45 Gbps at 10⁻⁶, 14.5 at 10⁻⁸ | 1.16, 11.6 Gbps |
| A request–reply loop (e.g. one synchronous gRPC call at a time) | `R ≤ S / RTT` per outstanding request | 1 MiB / 60.5 ms = 139 Mbps | 111 Mbps |

From [the LUX path result](../results/2026-11-23-lux-lb-path.md#what-the-rtt-means-for-tcp). This is why an `iperf3` TCP test of the WAN needs `-P 16` or more, and why the LB control plane (gRPC, sync packets) must stay off the data path. The LB's sync messages are periodic, not per event, so they don't limit the data rate.

## 3. Stages where latency caps the rate (fixed concurrency)

| Stage | N (in flight) | T (latency) | `R_max` | Measured |
|---|---|---|---|---|
| **Reassembler** (per receiver) | – | per-event cost | **~85k events/s** = 6.5 Gbps at 9.5 KB, 17.7 at 26 KB, 64.6 at 95 KB, 116 at 171 KB, 259 at 381 KB | 64 dequeue threads stayed at ~85k ev/s for 424 B and 95 KB events ([E2E bench §3](../results/2026-10-19-e2e-bench.md#3-dequeue-threads-the-hidden-bottleneck)) |
| **Dequeue threads** | `--deq` | per-event service time `t_svc` | `--deq × 8 · S / t_svc` | 4 threads stopped at 24.3 Gbps with 95 KB: 8.0k ev/s per thread, `t_svc` ≈ 125 µs. 16 threads reached 40.5 Gbps, 32 reached 61.2 |
| **DAOS writes** | `--deq × --inflight` (16 × 128 = 2048) | commit latency, p50 0.5–0.7 ms, p99 ~1 ms, max ~100 ms | far above line rate | At 40.8 Gbps of 381 KB: 13.4k ev/s × 0.5 ms = 7 in flight (p50), ~940 at a 70 ms tail; well under 2048 ([Perlmutter sweep](../results/2026-10-27-perlmutter-rate-sweep.md#latency-receive--committed-in-daos)) |
| **Reader** (`e2sar_daos_read --fetch`, one process) | 1 | ~62 µs per event | 1.2 Gbps at 9.5 KB | 16 TB nodes, one reader each: 14.8 Gbps sum ([TB nodes](../results/2026-11-11-tb-nodes-fetch.md)) |

Two things follow:

- **Small events hit the event ceilings first.** The LB 677 frame builders' 9.5 KB events can't exceed ~6.5 Gbps per receiver however good the network is; the cure is more receivers (the LB spreads events over them) or bigger events.
- The DAOS latency is not the limit at these rates. A latency that *looks* fine can hide a bottleneck one stage earlier: with too few dequeue threads, events wait up to a second in the Reassembler's queue while each DAOS write still takes 0.3 ms ([throughput metrics](../concepts/throughput-metrics.md)).

## 4. Stalls and buffers set the lossless rate

UDP doesn't slow the sender down when a stage stalls. A buffer in front of the stage absorbs the stall, and the stream is lossless only if

> **R × T_stall ≤ B**, i.e. the buffer covers the longest stall at the incoming rate.

| Buffer | B | How long it covers |
|---|---|---|
| Socket receive buffer, default 3 MB | 3 MB | 1.2 ms at 20 Gbps per node. LB 682 lost ~10% at the sockets (`RcvbufErrors`) |
| Socket buffer, `-b 1073741824` (kernel shows 2 GiB) | 2 GiB per socket | seconds at the per-socket rate. No `RcvbufErrors` since |
| NIC RX ring, 8192 packets | `8192 / pps` | **MTU 9000** (~8.7 KB per packet): 28 ms at 20 Gbps, 14 ms at 40. **MTU 1500** (~1.43 KB): 4.7 ms at 20 Gbps, 2.3 ms at 40 |
| NIC port buffer (`rx_discards_phy`) | on the card, small (check with `mlnx_qos -i enp1s0f1np1`) | sub-ms to ms; **the one that overflows in our runs** ([lossless ceiling](../concepts/lossless-ceiling.md)) |
| Reassembly timeout (`--timeout`, 500 ms default) | – | an event's packets must all arrive within it: `8 · S / R_stream` + jitter. A 2 GB file at 5 Gbps needs 3.2 s, so `--timeout 10000` |

The stalls are the latency **tail** (p99.99, max), not the median. Use the max per 1 s bin from `--timing-csv`, and the 1 s NIC counters, to find them.

### Packet rate

Per-packet work (DMA descriptors, interrupts, socket wake-ups) is what the NIC and kernel spend time on, so the packet rate matters more than the bit rate:

```
pps = R / (8 · P)      P = payload per packet: ~8.7 KB at MTU 9000, ~1.43 KB at MTU 1500
```

| Rate per node | MTU 9000 | MTU 1500 |
|---|---|---|
| 20 Gbps | 0.29 M pps | 1.75 M pps |
| 40 Gbps | 0.57 M pps | 3.5 M pps |

The LB 686 senders (LUX, JLab) used 1.43 KB packets: 21 Gbps per node was ~1.8 M pps, and 79% of the 40.8 Gbps run's losses were NIC port discards. The path LUX → LB → JLab passes MTU 9000 ([path check](../results/2026-11-23-lux-lb-path.md)), so a sender at MTU 9000 sends 6× fewer packets for the same rate.

## 5. Worked example: the ceiling today

Two receivers (ebpf2201, ebpf2203), pool sc-demo, ~381 KB events (LB 686):

| Stage | Limit | Value | Binding? |
|---|---|---|---|
| WAN + LB | measured, no ceiling found | ≥ 80 Gbps from one Perlmutter node (78.3 Gbps, 1.7×10⁻⁵ lost), 51.4 Gbps from one LUX node (5.6×10⁻³ lost, losses not split by cause) | no |
| WAN RTT | doesn't apply to UDP | – | no |
| Receiver NIC port, per node | measured lossless ceiling | **~25–35 Gbps per node**, so ~50–70 Gbps for two | **yes** |
| Reassembler, per node | 85k ev/s × 381 KB | 259 Gbps | no |
| Dequeue, 16–32 threads | measured | ≥ 61 Gbps at 95 KB | no |
| DAOS writes, sc-demo | measured | ≥ 40.8 Gbps (no limit reached) | not yet known |
| Socket buffers, 2 GiB | R × stall | seconds | no |

Measured: 40.8 Gbps with 1.09×10⁻³ lost, 79% at the NIC port ([40 Gbps run](../results/2026-11-12-203725UTC-lb686-40gbps-10tb.md)). The upper limit of the streaming path is set at JLab's receivers, not at the LCF or the WAN. To raise it: add receivers (each adds ~25–35 Gbps), send at MTU 9000, and use bigger events.

## 6. How to measure the inputs

| Input | Command or file |
|---|---|
| RTT and jitter to the LB data plane | `ping -c 100 -i 0.2 -M do -s 8972 <data IP>` (mdev = jitter); `tracepath -n <data IP>` for the path MTU. From the LCF node and from each receiver |
| Per-event latency per stage | `e2sar_daos_recv --timing-csv FILE` (`recv_s`, `commit_s`, `latency_ms`); E2E runs: `result.json` from `scripts/e2sar_e2e_analyze.py` |
| Event size and rate | `e2sar_daos_recv` progress lines, or the monitor JSON lines |
| Packet size and pps | `nic_*.log` (1 s `rx_packets_phy`), or `bytes / packets` from `ethtool -S` before and after |
| Buffers | `ss -uamn` (socket `rb`), `ethtool -g enp1s0f1np1` (ring), `mlnx_qos -i enp1s0f1np1` (port buffer) |
| Stalls and drops | `nic_*.log`: `rx_discards_phy`, `rx_out_of_buffer`, `Udp.RcvbufErrors`; [`scripts/e2sar_loss_timeline.py`](../reference/scripts.md) for loss by cause |
| WAN TCP baseline | `iperf3 -P 16` (the nightly job: `~/daq/iperf3-nightly/history.csv`) |

## 7. Finding the upper limit by experiment

1. Compute the ceilings above from your event size, MTU and measured latencies; the smallest is the prediction.
2. Ramp the sender (e.g. 10, 20, 40, 60, 80 Gbps, 60 s each, `e2sar_perf --mtu 9000`), with the NIC sampler running on every receiver.
3. At each step, compare the measured rate with the prediction. Where loss starts, read the loss cause: NIC port → add receivers or cut pps; sockets → bigger `-b`; reassembly → `--timeout` or `--threads`; upstream (no host drop) → the LCF, the WAN or the LB.
4. Write the result up with the RTT, MTU, event size and the binding stage.
