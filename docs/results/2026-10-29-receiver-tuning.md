---
type: Result
title: "Receiver tuning for layout v2 (2026-10-29)"
description: "The e2sar_daos_recv sweep of --threads, --deq, --inflight and --flush-us for small events, e2sar_ft files and 1 MiB events up to 80 Gbps, and the recommended settings."
tags: [tuning, e2sar_daos_recv, ebpf-testbed]
daos_layout: v2
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/STREAMING_WORKFLOW.md", title: "STREAMING_WORKFLOW.md at 0d85d00" }
---

# Tuning

The receiver settings swept on 2026-10-29 with [`scripts/e2sar_layout_sweep.py`](../../scripts/e2sar_layout_sweep.py): reassembly threads (`--threads`), dequeue threads (`--deq`), DAOS operations in flight per dequeue thread (`--inflight`) and, for small events, `--flush-us`. One receiver, `e2sar_daos_recv` on ebpf2203 into pool `sc26`; senders on ebpf2201.

**Node to node, not through the LB.** Without the LB nothing spreads one sender stream over several receiver ports, so each stream goes to its own port, which one reassembly thread serves: the reassembly threads equal the number of sender streams. Each run had its own container, destroyed after it. Loss is split into reassembly loss (`Lost in reassembly`, with the NIC's `rx_discards_phy`), enqueue loss (dequeue threads behind) and DAOS failures; the last two were 0 in every run.

## Results

*Italics* mark the recommended setting in each table.

**a. coda-fb-like: 20 KB events at 2.5 Gbps** (`e2sar_perf -l 20480`, one stream, 1 reassembly thread, `--inflight 32`; 300,000 events):

| `--deq` | `--flush-us` | Lossless | Events per batch | Latency p50 / p99 | Receiver CPU |
|---|---|---|---|---|---|
| *1* | *5000* | *yes* | *51* | *2.4 / 3.9 ms* | *1.5 cores* |
| *2* | *5000* | *yes* | *41* | *3.1 / 5.8 ms* | *1.6 cores* |
| 4 | 5000 | yes | 24 | 3.1 / 6.3 ms | 1.8 cores |
| 8 | 5000 | yes | 11 | 3.4 / 6.8 ms | 3.7 cores |
| 1–4 | 1000 | yes | 7–17 | 0.9 / 1.5 ms | 1.5–2.1 cores |
| 8 | 1000 | 2 events lost in reassembly | 3.6 | 1.2 / 2.4 ms | 3.9 cores |
| 1–8 | 20000 | yes | 39–51 | 2.4–11.5 / 3.9–23 ms | 1.5–3.4 cores |

**b. `e2sar_ft`, 2 GB EVIO files** (one stream, 1 reassembly thread, `--timeout 10000`):

| Run | Result |
|---|---|
| *1 file at 5 Gbps*; `--deq` 1–8, `--inflight` 16–128, `--max-write` 16M or 64M (*`--deq 4`, `--inflight 128`*) | *Lossless in every run.* Once reassembled, the 2 GB event is in DAOS 158–179 ms later (~100 Gbps); the settings make little difference |
| 4 files at 20 Gbps | All 4 lost in reassembly (1,605 NIC port discards) |
| 4 files unpaced | All 4 lost in reassembly (336 NIC port discards) |

**c1. The DAOS side alone: `--synthetic` 1 MiB events, unpaced** (30,000 events):

| `--deq` | `--inflight` 32 | `--inflight` 128 | Latency p50 |
|---|---|---|---|
| *4* | 170 Gbps | *182 Gbps* | *~1 ms* |
| 8 | 174 Gbps | 186 Gbps | 9–38 ms |
| 16 | 176 Gbps | 167 Gbps | 19–84 ms |
| 32 | 138 Gbps | 153 Gbps | 42–185 ms |

**c2. 1 MiB events, 8 `e2sar_perf` streams** (`--sockets 8` each, 8 reassembly threads, `--inflight 128`, 15 s per run):

| Total rate | Committed (*`--deq 8`*) | Events lost, `--deq` *8* / 16 / 32 | NIC port discards (`--deq 8`) | Receiver CPU (`--deq 8`) |
|---|---|---|---|---|
| 20 Gbps | 20.0 Gbps | *3* / 5 / 5 of 35,760 | 33 | 19 cores |
| 40 Gbps | 39.9 Gbps | *3* / 5 / 10 of 71,520 | 12 | 19 cores |
| 60 Gbps | 59.9 Gbps | *35* / 318 / 20 of 107,288 | 404 | 19 cores |
| 80 Gbps | 79.8 Gbps | *35* / 46 / 164 of 143,048 | 412 | 20 cores |

**c3. As c2 with 2 sender sockets per stream** (`e2sar_perf --sockets 2`), 8 or 16 streams and reassembly threads. Events not committed, of 71,520 (40 Gbps), 107,288 (60) or 143,048 (80):

| Reassembly threads | Total rate | `--deq 8 --inflight 32` | `--deq 8 --inflight 128` | `--deq 16 --inflight 32` | `--deq 16 --inflight 128` | Receiver CPU |
|---|---|---|---|---|---|---|
| 8 | 40 Gbps | *0* | 7 | 3 | 481 | *12 cores* (`--deq 8 --inflight 32`), 19–20 otherwise |
| 8 | 60 Gbps | *15* | 23 | 24 | 10 | *19–21 cores* |
| 8 | 80 Gbps | *15* | 30 | 36 | 10,825 ¹ | *20–21 cores* |
| 16 | 40 Gbps | 10 | 7 | 11 | 7 | 37–38 cores |
| 16 | 60 Gbps | 27 | 596 ² | 1,386 | 121 | 37–38 cores |
| 16 | 80 Gbps | 52 | 25 | 56 | 48 | 38–39 cores |

All lost events were reassembly losses with NIC port discards, except: ¹ 556,250 `rx_out_of_buffer` drops in that one run, while other tests ran on the sender node; ² 46 reassembly losses, the other 550 not accounted for by the receiver. Both are treated as outliers.

## Analysis

1. **DAOS is not the limit.** The layout commits 170–186 Gbps of 1 MiB events with 4–8 dequeue threads (c1), and through the network every event that reached the receiver was committed, up to 80 Gbps (c2). No run had enqueue loss or a failed DAOS write.
2. **The loss is in the NIC, as before.** Every lost event was a reassembly loss, and the NIC's port discards (`rx_discards_phy`) rose with it ([lossless-ceiling](../concepts/lossless-ceiling.md#diagnosing-the-port-discards)). From 60 Gbps it is ~0.03 % of 1 MiB events with 8 dequeue threads.
3. **Fewer dequeue threads are better.** More threads cost CPU (19 → 23 cores from 8 to 32 at 80 Gbps) and, at 80 Gbps, more loss (35 → 164 events), likely because they compete with the reassembly threads for cores. For small events they also split the stream into smaller batches: 51 events per batch with one thread, 11 with eight, for the same 2.5 Gbps.
4. **Batching makes small events cheap.** 15,600 events/s of 20 KB take 1.5 cores with 51 events per 1 MiB write. `--flush-us` trades latency for batch size: 1 ms gives sub-millisecond latency with 7–17 events per write; 20 ms adds latency and no larger batches once they are full.
5. **Fewer sender sockets, and no more reassembly threads than streams.** With 2 sockets per stream instead of 8, the loss at 80 Gbps halved (15 events instead of 35, c3 vs c2), and 40 Gbps was lossless at 12 cores. 16 reassembly threads (and 16 streams) doubled the CPU, to 37–39 cores, without losing less. `--inflight 32` did as well as 128.
6. **A large file is all or nothing.** A 2 GB `e2sar_ft` event is lost if any of its ~240,000 packets is, so one stream must be paced (5 Gbps was lossless) and the reassembly timeout must be longer than its transfer (3.4 s at 5 Gbps). Writing it to DAOS takes ~0.17 s, whatever the settings.

## Recommended settings

| Scenario | `--threads` | `--deq` | `--inflight` | Other |
|---|---|---|---|---|
| a. Small events (~20 KB), a few Gbps | 1 per stream | 1–2 | 32 | `--flush-us 5000`; `1000` for latency below 1 ms |
| b. Files with `e2sar_ft` | 1 per stream | 4 | 128 | Pace the sender (≤ 5 Gbps per stream); `--timeout` longer than one file's transfer |
| c. 1 MiB events, up to 80 Gbps | 1 per stream (port) | 8 | 32 | 8 streams, 2 sender sockets each (`--sockets 2`) |
