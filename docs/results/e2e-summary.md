---
type: Result
title: "End-to-end results: summary"
description: "The highest lossless rates node-to-node and through the LB, what loses events, what limits the rate, and latency, across the E2E runs of 2026-10-19 to 2026-10-28."
tags: [e2e, summary, ebpf-testbed]
daos_layout: v1
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/E2E_BENCHMARK.md", title: "E2E_BENCHMARK.md at 0d85d00" }
---

# End-to-end results: summary

> Measured on the old KV layout (v1), before layout v2 ([#31](https://github.com/JeffersonLab/e2sar-daos/pull/31)). The network findings still apply; the DAOS numbers are for v1.

How fast EVIO streaming time frames go from a sender on **ebpf2201**, through E2SAR (node-to-node or through the ESnet EJFAT load balancer), to `e2sar_daos_recv` on **ebpf2203** and into DAOS, without losing any. Also where each event's time goes on the way.
Measured on 2026-10-19 with E2SAR 0.4.0rc1, DAOS 2.6.5 (pool `sc26`) and LB 638 on `ejfat-lb.es.net`. The node-to-node rate sweep from 20 to 50 Gbps ([section 7](2026-10-20-e2e-n2n-sweep.md#7-node-to-node-rate-sweep-2050-gbps-2026-10-20)) is from 2026-10-20, and the LB rate sweep ([section 9](2026-10-21-e2e-lb-sweep.md#9-lb-rate-sweep-2045-gbps-2026-10-21)) from 2026-10-21 on LB 640. New LB runs should use ESnet's `stable4` LB (control plane `192.188.29.6:18048`, data plane `192.188.29.54`), see [lb-test](../howto/lb-test.md).

**The results on this page were measured with synthetic frames.** `e2sar_evio_send` then built its frames in memory ([the old synthetic frames](../howto/e2e-bench.md#the-events)). It now reads an EVIO file and sends the frames in it, so the bench sender needs `-F FILE`: add it to the commands below, and drop `-f`, to repeat a run.

## Summary

| | Result |
|---|---|
| **Highest lossless E2E, through the LB** | **24.3 Gbps** E2E (25.2 received) on LB 640, 2026-10-21: 1 of 2 runs at 25 Gbps, and every run at 30–45 Gbps lost 3–20 events, all in the first 0.5 s. See [section 9](2026-10-21-e2e-lb-sweep.md#9-lb-rate-sweep-2045-gbps-2026-10-21). On 2026-10-19 (LB 638, 16 dequeue threads), 35 Gbps (33.7 E2E) was lossless in 2 of 2 runs |
| Highest lossless E2E, node-to-node | **29.9 Gbps** E2E (30.3 received), in 1 of 6 runs at 30 Gbps. 25 Gbps: 2 of 6. No run at 35–50 Gbps was lossless, and no rate was lossless in every run. See [section 7](2026-10-20-e2e-n2n-sweep.md#7-node-to-node-rate-sweep-2050-gbps-2026-10-20) |
| KV vs files through pil4dfs, 95 KB events | **KV.** pil4dfs (one file per event, `-S pil4dfs`, 32 writers) takes 0.9–1.7 ms per file and commits at most 18–33k events/s (13–25 Gbps). From 20 Gbps up it falls behind, and events queue for seconds. KV takes 0.23–0.30 ms and keeps up to 50 Gbps. See [section 7](2026-10-20-e2e-n2n-sweep.md#kv-vs-files-through-pil4dfs) |
| Node-to-node after the first 0.5 s | **No loss up to 50 Gbps in 39 of 42 runs.** Almost all lost events were queued in the first 0.1 s of a run, while the receiver's first DAOS writes are slow |
| Lossless in every run | **20 Gbps through the LB (3 of 3)**, the new defaults. Node-to-node the same settings passed 1 of 3 |
| E2SAR alone (`e2sar_perf`, no DAOS), 16 sockets → 32 threads | **Lossless at 10 Gbps through the LB** (4 of 4). At 20 Gbps every LB run lost 0.2–0.4%, while node-to-node 20 Gbps was lossless. From 30 to 90 Gbps both paths lose about 0.01–0.2% per run, mostly at start-up. `recvmmsg` avoids the 1–2% node-to-node spikes through the LB. See [section 10](2026-10-26-e2sar-perf-n2n-vs-lb.md#10-e2sar_perf-without-daos-node-to-node-vs-lb-1090-gbps-2026-10-26-and-2026-10-28) |
| Highest with any loss | 61.2 Gbps received (59.8 E2E), node-to-node, 99.96% of events delivered |
| What loses events | Frames dropped at ebpf2203's NIC port (`rx_discards_phy`). The LB path smooths the bursts, so loss starts later through it than node-to-node. Never DAOS, never the sockets. Why the port overflows is not settled yet: see [Diagnosing the port discards](../concepts/lossless-ceiling.md#diagnosing-the-port-discards) |
| What limits the rate | 1. The DAOS dequeue threads: 4 cap at ~24 Gbps, 16 reach 40, 32 reach 61. 2. Then the Reassembler at ~85k events/s, whatever the event size |
| The DAOS sink alone | Without a sender (`--synthetic`), the KV sink at the defaults commits **92–103 Gbps** of 95 KB events and 183 Gbps of 1.5 MB events (the IB link). DAOS isn't what limits E2E. See [section 7](2026-10-20-synthetic-sink.md#8-the-sink-without-a-sender---synthetic) |
| Latency at the defaults | Queued at the sender → committed in DAOS: **0.3–0.5 ms p50** node-to-node, most of it the DAOS write. **75.6 ms p50** through the LB, almost all of it the WAN round trip to ESnet. See [latency per stage](2026-10-19-e2e-bench.md#latency-per-stage-ms-same-runs) |
| Latency node-to-node, 20–50 Gbps | After the first 0.5 s: **0.2–1.0 ms p50, 0.4–2.6 ms p99** at every rate, DAOS 0.23–0.30 ms of it. The first 0.2–0.4 s of each run wait up to ~170 ms, which is the whole-run p99 of 116–167 ms. See [section 7](2026-10-20-e2e-n2n-sweep.md#latency-per-stage-node-to-node-ms) |
| iperf3 baseline | Partial (2026-10-15, DAOS idle). TCP into ebpf2203 reaches 98 Gbps, so the link, port and PCIe path sustain about line rate. UDP stopped at the iperf3 sender's ~20 Gbps with 0 loss, so there is no zero-loss UDP rate above 20 Gbps yet. See [what the evidence says](../concepts/lossless-ceiling.md#what-the-evidence-says-so-far) |
