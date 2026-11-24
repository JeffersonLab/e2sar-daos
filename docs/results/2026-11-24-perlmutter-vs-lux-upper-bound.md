---
type: Result
title: "Perlmutter ~80 Gbps vs LUX ~46–51 Gbps through the LB: what the runs show about the upper bound (analysis, 2026-11-24)"
description: "Re-analysis of the 2026-10-27 Perlmutter and 2026-10-28/29 LUX e2sar_perf runs (1 MiB events, MTU 9000, same two receivers and settings). Confirmed: Perlmutter 78.3 and 79.8 Gbps (≤1×10⁻⁴ lost); LUX at most 51.4 and 45.9 Gbps (0.56% and 1.0% lost). The bound is R = min(sockets × r₁, sender NIC/egress, path, receivers). Each in-flight 1 MiB event arrives at ~2.5 Gbps at both sites (Little's law on the completion order), so 80 Gbps needs ~32 events in flight and 16–20 send sockets cap at ~40–50 Gbps. The receivers took 40 Gbps per node from Perlmutter, so JLab didn't cap LUX. The LUX arrivals had gaps (6–23% idle 1 ms bins, 11–19 ms max gap) where Perlmutter at 80 had none, so the LUX sender was paced below its ceiling: 46–51 Gbps is what LUX sent, not yet LUX's limit. The sender's --sockets and --rate weren't kept; the runs that would settle it are listed."
tags: [perlmutter, lux, nersc, olcf, lb, ejfat, e2sar_perf, throughput, upper-bound, parallelism, sockets, littles-law, loss, analysis, todo]
sources:
  - { id: pm, resource: "2026-10-27-perlmutter-rate-sweep.md", title: "Perlmutter → ESnet LB → two ebpf receivers, 10–80 Gbps" }
  - { id: summary, resource: "2026-11-12-rate-summary.md", title: "Rates and losses by setting" }
  - { id: sender, resource: "2026-10-19-bottleneck-e2sar-ft.md#sender-one-event-is-sent-by-one-thread", title: "Sender: one event is sent by one thread" }
  - { id: howto, resource: "../howto/throughput-from-latency.md", title: "The upper limit of LCF → LB → JLab streaming" }
---

# Perlmutter ~80 Gbps vs LUX ~46–51 Gbps through the LB: what the runs show about the upper bound

**Question:** Perlmutter reached ~80 Gbps through the ESnet LB into JLab, LUX only ~46 Gbps. Is that a real limit, where is it, and what is the upper bound for each site?

The method is general and written up step by step, independent of the site, in [the how-to §7](../howto/throughput-from-latency.md#7-the-analysis-method-for-any-sender-site). This page applies it to Perlmutter and LUX.

**Short answer:** Both numbers are confirmed. The JLab receivers were not what held LUX back: they took 40 Gbps per node from Perlmutter, against 23–26 from LUX. By Little's law, each 1 MiB event arrives at ~2.5 Gbps at both sites, so the rate is set by how many events the sender keeps in flight: about **sockets × 2.5 Gbps**. The LUX traffic arrived with gaps, which a sender at its limit doesn't leave. So 46–51 Gbps is what the LUX sender was set to send, not LUX's ceiling. The LUX ceiling hasn't been measured yet. §7 lists the runs that would measure it.

## 1. The runs

Same receivers in both: `e2sar_daos_recv --withcp` on ebpf2201 and ebpf2203, 16 receive ports each, 32 dequeue threads × 256 puts, 1 GiB socket buffers, pool `sc26` (gone since). Sender `e2sar_perf -l 1048576 --mtu 9000` on one compute node. **`--rate` and `--sockets` were not kept** for either site.

| Run | Site | Events | Rate (Gbps) | Per node | Lost | `rx_discards_phy` / packets |
|---|---|---|---|---|---|---|
| `pm_20261027_222315` | Perlmutter | 10⁶ | **79.8** | 39.9 + 39.9 | 9.7×10⁻⁵ | 8.9×10⁻⁶ (1,045 / 118M) |
| `pm_20261027_221922` | Perlmutter | 10⁶ | **78.3** | 39.2 + 39.1 | 1.7×10⁻⁵ | 1.3×10⁻⁶ |
| `pm_20261027_222633` | Perlmutter | 10⁶ | 39.6 | 19.8 + 19.9 | 4.9×10⁻³ | 3.1×10⁻⁴ |
| `pm_20261027_223531` | Perlmutter | 10⁶ | 19.9 | 10.0 + 10.0 | 3.4×10⁻⁴ | 1.1×10⁻⁵ |
| `pm_20261027_225105` | Perlmutter | 10⁶ | 10.0 | 5.0 + 5.0 | 1.3×10⁻⁴ | 3.2×10⁻⁶ |
| `lux_20261028_161516` | LUX | 10⁵ | **51.4** | 25.6 + 25.8 | 5.6×10⁻³ | 1.3×10⁻³ |
| `lux_20261028_161745` | LUX | 10⁵ | **45.9** | 22.9 + 23.0 | 1.0×10⁻² | 2.5×10⁻³ |
| `lux_20261029_124858` | LUX | 3.9×10⁵ (stopped) | 30.6 | 15.3 + 15.3 | 1.2×10⁻² | 2.8×10⁻³ |
| `lux_20261029_125324` | LUX | 10⁶ | 30.3 | 15.1 + 15.2 | 1.3×10⁻² | 2.7×10⁻³ |

Sources: `pm-bench/summary.csv` and `lux-bench/lux_parsed.json` on the laptop (receiver logs and NIC diffs). `lux_20261028_161948` (1,448 events at 0.1 Gbps) is left out. The LB 686 run at 45 Gbps (2026-11-12) isn't in the table: it came from frame builders that aren't ours, at LUX and JLab together, at MTU 1500.

So, confirmed: **Perlmutter 78–80 Gbps with ≤10⁻⁴ lost; LUX 45.9–51.4 Gbps at best, losing 0.6–1.0%.** Every loss in every run came with NIC port discards on the receiver, never socket or ring drops.

## 2. The bound, with parallelism

UDP has no feedback, so the rate that arrives is the rate that was sent, minus losses. The highest rate is the smallest of:

```
R_max = min( N_s · r₁ ,  C_sender NIC/egress ,  C_WAN+LB ,  Σ_receivers C_recv )
          └ sender ┘
```

- **Sender parallelism (`N_s · r₁`).** E2SAR's Segmenter gives each event to one send thread, which sends all of its frames at that thread's speed `r₁`. `--rate` only waits *between* events ([sender analysis](2026-10-19-bottleneck-e2sar-ft.md#sender-one-event-is-sent-by-one-thread)). With `N_s` sockets/threads, at most `N_s` events are in flight. By **Little's law**, the events in flight are `K = λ · T_evt = R / r₁`, so **R ≤ N_s · r₁**.
- **Receivers.** The LB spreads events over the receivers, and each sender socket (its entropy) maps to one receive port. So `N_s` also sets how many receive ports per node are busy, up to 16.
- **The WAN round trip is not in the bound.** Nothing waits for an acknowledgement ([how-to §2](../howto/throughput-from-latency.md#2-the-wan-rtt-doesnt-cap-udp-streaming)).

## 3. Measuring r₁ from the receivers: Little's law on the completion order

`e2sar_perf` numbers events in sequence, and each receiver's `--timing-csv` records when each event completed. If events went out one at a time, they would complete in order. With `K` events in flight at once, each event completes up to its own arrival time `T_evt` away from its place in the order. So the spread of that lag measures `T_evt`, and Little's law gives `K = λ · T_evt` and `r₁ = R / K = 8·S / T_evt`. Each node is measured against its own clock, so no clocks are compared across hosts.

From [`scripts/e2sar_arrival_stats.py`](../../scripts/e2sar_arrival_stats.py):

| Run | Per node | Lag p50 | Lag p90 = `T_evt` | `K` per node | `r₁` |
|---|---|---|---|---|---|
| Perlmutter 79.8 | 39.9 Gbps | 2.24–2.31 ms | 3.28–3.33 ms | 15.6–15.8 | 2.5–2.6 Gbps |
| Perlmutter 78.3 | 39.2 | 2.30 | 3.29–3.35 | 15.4–15.6 | 2.5 |
| Perlmutter 39.6 | 19.8 | 2.20 | 3.33 | 7.9 | 2.5 |
| Perlmutter 19.9 | 10.0 | 1.97–1.99 | 3.48–3.52 | 4.1–4.2 | 2.4 |
| Perlmutter 10.0 | 5.0 | 1.47 | 3.45–3.48 | 2.1 | 2.4 |
| LUX 51.4 | 25.7 | 2.25 | 3.26–3.27 | 10.0 | 2.6 |
| LUX 45.9 | 23.0 | 2.20–2.22 | 3.36–3.37 | 9.2 | 2.5 |
| LUX 30.3–30.6 | 15.2 | 2.13–2.19 | 3.45–3.50 | 6.2–6.3 | 2.4 |

Reasoning:

1. **`T_evt` ≈ 3.3–3.5 ms in every run, at both sites, from 5 to 80 Gbps.** It doesn't change with the rate, so it is a property of one event's trip, not of the load. At 10 Gbps, events start every 0.84 ms but each takes ~3.5 ms to arrive, so ~4 overlap.
2. **So `r₁` ≈ 8 × 1 MiB / 3.3 ms ≈ 2.5 Gbps per in-flight event, the same at Perlmutter and LUX** (within ±5%). One in-flight event is no faster from Perlmutter than from LUX.
3. **The rate is then set by the number of events in flight:** `K_total = R / r₁` ≈ **32 at 80 Gbps** (Perlmutter), **~20 at 51.4** and **~18 at 45.9** (LUX), ~12 at 30.
4. With one send thread per socket, **80 Gbps needs ≥ 32 events in flight. 16 sockets would cap at ~40 Gbps and 20 at ~50 Gbps,** which is where the LUX runs sit. If the LUX sender used 16–20 sockets, that alone explains 46–51 Gbps.

**Caveat.** The lag is measured at the receiver, so `T_evt` also contains any time an event waits there before its completion is recorded. That makes it an upper bound, and `r₁` a lower bound. On ebpf2201, one send thread reached 10–11 Gbps (one large event, `e2sar_ft`). If LCF threads are as fast, the 3.3 ms is mostly WAN/LB spread or receiver-side, and `K` is ~4× smaller. That changes the numbers in step 4 but not the reasoning: **R = events in flight × per-event rate.** The `--sockets` sweep in §7 settles `r₁` directly.

## 4. The receivers did not cap LUX

- The same two receivers took **39.9 Gbps per node** from Perlmutter with 8.9×10⁻⁶ of packets discarded at the port. LUX sent **15–26 Gbps per node**. A receiver limit can't be the reason LUX stayed below 51 Gbps.
- So with continuous traffic, `Σ C_recv` ≥ 80 Gbps for the two nodes ([the lossless ceiling](../concepts/lossless-ceiling.md) is about bursts, see §5).

## 5. The LUX sender was paced, so it was below its ceiling

A sender at its ceiling keeps every send thread busy and leaves no gaps. A sender paced by `--rate` waits between events, and the receivers see on/off arrivals. Per node, in 1 ms bins:

| Run | Mean | Idle 1 ms bins | Longest gap | Lost |
|---|---|---|---|---|
| Perlmutter 79.8 / 78.3 | 39.2–39.9 Gbps | **0.6%** | **2.5–3.1 ms** | ≤ 1×10⁻⁴ |
| Perlmutter 39.6 | 19.8 | 7.4% | 130–260 ms | 4.9×10⁻³ |
| Perlmutter 19.9 | 10.0 | 27% | 8–262 ms | 3.4×10⁻⁴ |
| Perlmutter 10.0 | 5.0 | 52% | 83–85 ms | 1.3×10⁻⁴ |
| LUX 51.4 | 25.7 | **6.2%** | **10.5–11.6 ms** | 5.6×10⁻³ |
| LUX 45.9 | 23.0 | 10% | 11.3–12.7 ms | 1.0×10⁻² |
| LUX 30.3–30.6 | 15.2 | 22–23% | 14.6–19.4 ms | 1.2–1.3×10⁻² |

Reasoning:

1. **Only the two Perlmutter runs at ~80 Gbps were continuous** (0.6% idle, ≤3 ms gaps). Those are the runs where the sender was at or near its ceiling, and they lost the least.
2. **Every LUX run had gaps**, 6% idle even at 51.4 Gbps. The LUX sender was waiting between events: paced by `--rate` (or by something on the LUX side), not saturated. **Its ceiling is above 51.4 Gbps, or at least wasn't reached in these runs.** The two 51.4 and 45.9 Gbps runs sent 10⁵ events. If they followed the earlier convention of rate × 1,000 events, the request was 100 Gbps, and LUX delivered about half, which would be a sender limit after all. Only the LUX sender log or command can say which.
3. **Gappy arrivals lose more.** Perlmutter at 39.6 Gbps lost 0.49% and at 80 lost 10⁻⁴. LUX, always gappy, lost 0.6–1.3%, with 4–9× more port discards per packet than Perlmutter at 40. When a gap ends, the queued events go out together at full speed, and the burst overflows the receiver's port buffer. The higher losses from LUX look like a pacing effect at JLab's NIC, not a LUX path problem. The losses came in steps every ~10 s at both sites; that may be when E2SAR updates its lost-event count rather than when the packets were dropped.

## 6. The upper bound, per site

| Term | Perlmutter | LUX |
|---|---|---|
| `N_s · r₁` (sender parallelism) | ≥ 80 Gbps shown (~32 events in flight) | ≥ 51.4 shown (~20 in flight); ceiling not reached |
| Sender NIC / site egress | `hsn0` 200 Gb/s | `frontend.4`, speed **not recorded** |
| WAN + LB | ≥ 80 shown | ≥ 51.4 shown; RTT 60.5 ms doesn't matter for UDP |
| Receivers, 2 nodes | ≥ 80 continuous (40 per node) | same receivers |
| **Upper bound today** | **≥ 80 Gbps** (no term found that caps it yet; the next likely one is the receivers' NIC ports at ~40–50 per node) | **≥ 51.4 Gbps; unknown above.** Not limited by JLab |

To go past 80 Gbps from either site, add receivers: each node took ~40 Gbps of continuous traffic.

## 7. To do: the runs that would settle it

- [ ] **Measure `r₁` and the LUX ceiling directly:** on one LUX node, `e2sar_perf --mtu 9000 -l 1048576 -n 20000 --rate -1` (or a rate above the expected ceiling) with `--sockets 1, 2, 4, 8, 16, 32`. **Keep the sender log** (`Estimated goodput`, the settings). The rate should grow as `N_s · r₁` and flatten at the sender NIC or egress. Do the same on Perlmutter.
- [ ] **Record the sender's settings in every run** (`--rate`, `--sockets`, node name). The 2026-10 runs lost them, which is why §3 had to infer `K`.
- [ ] **LUX NIC speed:** `cat /sys/class/net/frontend.4/speed`, or `ethtool` on its parent, and `ethtool -S` TX drops after a run.
- [ ] **A continuous LUX run:** `--rate` at or just under the plateau found above, with enough sockets, to see whether LUX losses drop to Perlmutter's 10⁻⁴ when the gaps go away (§5).
- [ ] **Past 80 Gbps:** three or four receivers, with `--sockets` ≥ `R / r₁`.

## Reproduce

```bash
# laptop, from ~/Documents/DAQ
python3 e2sar-daos/scripts/e2sar_arrival_stats.py pm-bench/pm_2026* lux-bench/lux_2026*
```

The per-event CSVs are also on the ebpf nodes: `~/e2sar_run/pm/pm_20261027_*/` (Perlmutter). The LUX ones are on the laptop only (`lux-bench/`).
