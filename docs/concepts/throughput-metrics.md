---
type: Concept
title: "Throughput and latency metrics"
description: "Definitions of sender goodput, ingest, DAOS commit, received and E2E rates, the per-stage latencies, and the loss counters used in every result."
tags: [metrics, throughput, latency, loss]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/BOTTLENECK_ANALYSIS.md", title: "BOTTLENECK_ANALYSIS.md at 0d85d00" }
  - { id: e2e, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/E2E_BENCHMARK.md", title: "E2E_BENCHMARK.md at 0d85d00" }
---

# Throughput and latency metrics

All rates are Gbps (10⁹ bit/s) of event payload. The first section defines the rates `e2sar_ft` and `e2sar_daos_recv` print; the second the rates and latency stages of the end-to-end bench ([e2e-bench](../howto/e2e-bench.md)).

## Throughput metrics

These definitions apply to every results table in this directory. All rates are in Gbps (10⁹ bit/s) of **payload**, meaning the file bytes. E2SAR, LB, UDP and IP headers aren't counted.

| Metric | Printed by | Formula | What it tells you |
|---|---|---|---|
| **Sender goodput** | `e2sar_ft`: `Estimated goodput (Gbps)` | Total size of the files queued × 8 / *send time*. Send time runs from just before the first file is queued until every file has been read and segmented and the send threads have stopped (`bin/e2sar_ft.cpp`) | How fast the sender pushed data out, including reading the files. It's measured only on the sender, so it says nothing about whether the packets arrived. With `--withcp`, the LB registration at startup is outside this window |
| **Ingest** | `e2sar_daos_recv`: `Ingest throughput` | Bytes of all fully reassembled events × 8 / *receive window*. The receive window runs from when the Reassembler hands over the first complete event to when it hands over the last one | What the network plus reassembly delivered to the application |
| **DAOS commit** | `e2sar_daos_recv`: `DAOS commit throughput` | Bytes committed × 8 / *commit window*. The commit window runs from the first event handed over until the last event's metadata key is committed | Data safely stored in DAOS. If it's close to ingest, DAOS kept up |

Caveats on the receiver metrics:
- Both windows start when the first event is *complete*, so that event's own transfer time isn't counted, but its bytes are. With few events, both rates come out too high.
- With one event the receive window is 0, and ingest prints `n/a`. The commit window then covers only the DAOS write tail, so DAOS commit is meaningless. For example, a single 2.1 GB event showed "120 Gbps".
- When several events finish at nearly the same time, as with parallel senders, the windows are tiny and the rates come out far too high (tables 6–7).
- For a fair comparison, send tens of events per run, or use an **end-to-end** rate: total bytes × 8 / (sender start → last commit). This needs synchronized clocks. On the ebpf nodes chrony keeps them within ~20 µs. That interval also includes `e2sar_ft` startup and, with `--withcp`, the LB control-plane calls (~1 s over the WAN).

Loss counters, read before and after each run:
- *`Lost in reassembly`*: events the receiver started but couldn't complete within `--timeout`. Any single lost packet loses the whole event. An event whose packets *all* went missing isn't counted here. It's simply absent from `Events committed`.
- *`UdpRcvbufErrors`*: kernel UDP socket overflow on the receiver (`nstat`, or `/proc/net/snmp`).
- *NIC drops*: `rx_out_of_buffer` (RX ring full) and `rx_discards_phy` (NIC port buffer) from `ethtool -S enp1s0f1np1`.
  These counters cover all traffic on the NIC, including other users' tests, so read them only when the nodes are otherwise idle.

## What is measured

All rates are Gbps (10⁹ bit/s) of event payload, meaning the EVIO records. E2SAR, LB, UDP and IP headers are not counted.

| Metric | Formula | Where |
|---|---|---|
| **Received** | Bytes of all committed events except the first one to commit / (last commit − first commit) | Receiver only. The steady rate at which events land in DAOS |
| **E2E** | All committed bytes / (first event queued at the sender → last event committed in DAOS) | Both hosts' clocks |
| Sender | Bytes sent / (first queued → last packet sent) | Sender only |

Latency per event, matched on (event number, dataId):

| Stage | From → to | What's in it |
|---|---|---|
| **sender** | Handed to the Segmenter → its last packet sent | Waiting in the Segmenter's queue, segmenting, sending |
| **network** | Last packet sent → a dequeue thread takes the reassembled event | Wire (+ LB and WAN), reassembly, and **waiting in the Reassembler's queue for a free dequeue thread** |
| **daos** | Taken by a dequeue thread → its metadata key committed | The chunk puts and the metadata put |
| **total** | Handed to the Segmenter → committed | Everything |

- Both clocks are chrony-synced to JLab NTP, with offsets of 0.03–0.09 ms, so the cross-host stages are good to about ±0.2 ms.
- The "network" stage includes the wait for a dequeue thread. When the dequeue threads are the bottleneck, it's that wait that grows, not "daos". See [the dequeue thread sweep](../results/2026-10-19-e2e-bench.md#3-dequeue-threads-the-hidden-bottleneck).
- With an unpaced sender, events wait in the sender's queue, so "sender" and "total" mostly measure the backlog. Use a paced sender (`-r`) for latency.
- `latency_steady_ms` in `result.json` ("after 0.5 s" in the summary) has the same stages for the events queued 0.5 s or more after the first one (`e2sar_e2e_analyze.py --warmup`). The start of every run is slow (see [section 7](../results/2026-10-20-e2e-n2n-sweep.md#warm-up-at-the-start-of-each-run)), and in a run of 1.5–4 s it decides p90 and p99. `lost_queued_s` lists when the lost events were queued.
