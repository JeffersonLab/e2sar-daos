---
type: Result
title: "The sink without a sender (--synthetic), 2026-10-20"
description: "What e2sar_daos_recv --synthetic commits with no network: 92–103 Gbps of 95 KB events at the defaults, 183 Gbps of 1.5 MB events; dfuse and pil4dfs for comparison."
tags: [synthetic, daos, ebpf-testbed]
daos_layout: v1
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/E2E_BENCHMARK.md", title: "E2E_BENCHMARK.md at 0d85d00" }
---

# 8. The sink without a sender (`--synthetic`)

> Measured on the old KV layout (v1), before layout v2 ([#31](https://github.com/JeffersonLab/e2sar-daos/pull/31)). The network findings still apply; the DAOS numbers are for v1.

`e2sar_daos_recv --synthetic N` stores N events generated in memory, with no network and no Reassembler. Everything from the dequeue threads on is the same receiver code ([bin/README.md](../reference/e2sar_daos_recv.md#synthetic-mode-the-storage-ceiling-without-a-sender)). Unpaced, it shows the most the sink can commit with the given settings, which no end-to-end run with those settings can beat. It needs a sink (`--cont-name`, or `--dfuse`) and can't be combined with `--dryrun`.
`fio --ioengine=dfs` isn't a substitute: it writes byte ranges of a few large files, while the receiver does one KV put (or one file) per event, so per-operation costs that limit the receiver don't show up in fio's Gbps.

Measured on 2026-10-20 on ebpf2203, idle testbed, the same debug build as the E2E runs. The DAOS engines are remote, behind the 200 Gb/s HDR port. Every run committed every event.

| Sink | Event | `--deq` | Events | Commit Gbps | Events/s | Event latency p50 / p99 |
|---|---|---|---|---|---|---|
| kv | 95 KB | 4 | 100k ×2 | 27.6, 27.4 | 35k | 0.33 / 0.5–1.3 ms |
| kv | 95 KB | **16 (default)** | 100k ×2, 500k | 103, 93, 92 | 118–132k | 0.33–0.43 / 2.6–16 ms |
| kv | 95 KB | 32 | 100k ×2, 500k | 106, 111, 130 | 137–167k | 0.49–0.52 / 0.9–21 ms |
| kv | 95 KB, `--chunk 0` | 16 | 100k ×2 | 97, 98 | 125k | 0.33 / 1.0–1.3 ms |
| kv | 1.5 MB | 16 | 10k ×2, 50k | 158, 166, **183** | 13–15k | 84 / 185–201 ms |
| dfuse | 95 KB | 16 | 50k | 2.5 | 3.2k | 5.2 / 10 ms |
| pil4dfs | 95 KB | 16 | 50k | 12.6 | 16k | 0.98 / 1.7 ms |
| dfuse | 1.5 MB | 16 | 5k | 35 | 2.8k | 4.7 / 42 ms |
| pil4dfs | 1.5 MB | 16 | 5k, 25k | 160, 165 | 13k | 1.0–1.2 / 1.8–3.4 ms |
| kv, paced `--rate 20` | 95 KB | 16 | 100k | 20.0 | 26k | 0.33 / 0.98 ms |

What this says about the E2E results:
- **The DAOS KV sink isn't what limits E2E.** At the defaults it commits 92–103 Gbps of 95 KB events, about 4× the highest lossless E2E rate. The limits are on the network side: the port discards and the Reassembler.
- **The 4-thread cap in [section 3](2026-10-19-e2e-bench.md#3-dequeue-threads-the-hidden-bottleneck) is the sink.** 4 dequeue threads commit 27.5 Gbps without a sender and 24.3 with one. That's about 8.8k events/s per thread, with one blocking `recvEvent`, two puts and their completions per event.
- **The ~85k events/s ceiling is the Reassembler, not DAOS.** Without it, 32 threads commit 167k events/s.
- **1.5 MB events reach the IB link** (183 Gbps ≈ 91% of 200 Gb/s). Their latency is queueing: 16 × 128 puts in flight at once, unpaced. At an E2E rate DAOS takes ~0.5 ms per event.
- **Plain dfuse is the sink's own limit**, the same 3.2k files/s as in the earlier dfuse sweep. pil4dfs reaches 16k files/s with 95 KB events and about the KV rate with 1.5 MB events.
- **Paced at 20 Gbps, DAOS adds 0.33 ms p50**, the same as the "daos" stage in the node-to-node E2E runs. E2SAR traffic on the shared adapter doesn't slow the DAOS writes.
- `--chunk 0` makes no difference at 95 KB: both layouts are one data put and one metadata put.

`--synthetic --rate R` is also a cleaner DAOS load for run 2 of the [diagnosis plan](../concepts/lossless-ceiling.md#runs-cheapest-first) than fio: the same write pattern as the receiver, at a fixed rate.
