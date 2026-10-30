---
type: Result
title: "e2sar_perf without DAOS: node-to-node vs LB, 10–90 Gbps (2026-10-26 and 2026-10-28)"
description: "E2SAR's own e2sar_perf at both ends, no DAOS: loss per rate, node-to-node and through LB 86, with and without recvmmsg."
tags: [e2sar_perf, lb, node-to-node, ebpf-testbed]
daos_layout: none
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/E2E_BENCHMARK.md", title: "E2E_BENCHMARK.md at 0d85d00" }
  - { id: runs, resource: "ebpf testbed: ~/e2sar_perf_tune (logs/F_T32_*, logs/L_T32_*)", title: "Scripts and logs of the sweep" }
---

# 10. `e2sar_perf` without DAOS: node-to-node vs LB, 10–90 Gbps (2026-10-26 and 2026-10-28)

This sweep uses E2SAR's own `e2sar_perf` at both ends, so there is **no DAOS and no `e2sar_daos_recv`**. It measures only the E2SAR transport into ebpf2203, with and without the `recvmmsg` receive optimization.

Settings, the same on both days:
- Sender on ebpf2201: 16 sockets, 1 MiB events, rate × 1000 events per run (about 8.5 s of sending), `--smooth`, 1 GiB socket buffer.
  A second sender adds a 2 Gbps keep-alive stream (data id 1111), started 3 s before the main stream (data id 4321) and running past its end.
- Receiver on ebpf2203: `e2sar_perf -r` with 32 threads (ports 20600–20631), `--deq 4`, 1 GiB socket buffer, 500 ms reassembly timeout.
- Two modes: plain, and `-o recvmmsg --rcviovecsize 1024`.
- Two runs (a, b) per rate and mode.

The two paths:
- **Node-to-node (2026-10-26):** no control plane. Only the 16 even ports get traffic, one per send socket.
- **LB (2026-10-28):** LB instance 86 on `stable4`, data plane `192.188.29.48`, 71–76 ms RTT, no other sender or worker registered, both nodes otherwise idle.
  - The receiver uses `--withcp` and registers 32 ports. The LB spreads events over all of them.
  - The main sender uses `--withcp`, so it sends sync packets. The keep-alive relies on `129.57.178.85` being added to the allowlist by hand before each run.

The scripts and logs are in `~/e2sar_perf_tune` on the shared testbed home:
- Node-to-node: `recv.sh` and `send_overlap.sh`, logs `logs/F_T32_*`.
- LB: `recv_lb2.sh` and `send_lb.sh`, logs `logs/L_T32_*`.

Each cell is the events of the main stream lost in reassembly, as run a · run b, with the share of the events sent. *crash* means that run produced no result, because the receiver died at startup.

| Rate (Gbps) | Node-to-node, plain | **LB, plain** | Node-to-node, recvmmsg 1024 | **LB, recvmmsg 1024** |
|---|---|---|---|---|
| 10 | 0 · *crash* | **0 · 0** | 0 · 0 | **0 · 0** |
| 20 | 0 · *crash* | 78 (0.39%) · 40 (0.20%) | *crash* · 0 | 42 (0.21%) · 77 (0.39%) |
| 30 | 17 (0.06%) · 11 (0.04%) | 16 (0.05%) · 54 (0.18%) | 9 (0.03%) · 46 (0.15%) | 5 (0.02%) · **0** |
| 40 | 48 (0.12%) · 48 (0.12%) | 16 (0.04%) · 4 (0.01%) | **840 (2.1%)** · 52 (0.13%) | 6 (0.02%) · 5 (0.01%) † |
| 60 | 46 (0.08%) · 32 (0.05%) | 15 (0.03%) · 26 (0.04%) | *crash* · 45 (0.08%) | 21 (0.04%) · **266 (0.44%)** |
| 80 | 47 (0.06%) · *crash* | 34 (0.04%) · 17 (0.02%) | **1951 (2.4%)** · 60 (0.08%) | 27 (0.03%) · 35 (0.04%) |
| 90 | 41 (0.05%) · 48 (0.05%) | 25 (0.03%) † · 27 (0.03%) | **1226 (1.4%)** · 46 (0.05%) | 52 (0.06%) · 35 (0.04%) |
| Sent at 80 / 90 | 75 / 87 | 78–79 / 89.6 | 76 / 88 | 80.4 / 89.5–89.9 |

† A rerun (`r40c`, `r90c`). In the first try, the receiver stopped a few seconds after registering, before any data arrived, without an error message and without a segfault in `dmesg`.

The LB runs also lost 2–109 keep-alive events each, counted separately and left out of the table.
Most of them are probably keep-alive events cut off when the main sender removed `129.57.178.85` from the allowlist on exit, with the keep-alive still running.

- **Lossless only at 10 Gbps through the LB**: all 4 runs. recvmmsg also had one lossless run at 30.
  Node-to-node, 20 Gbps was lossless in both runs that didn't crash.
- **At 20–30 Gbps through the LB, the loss is spread over the whole run.** The lost event ids reach ~20,000, and ebpf2203's NIC discarded 35–640 frames per run (`rx_discards_phy`).
  This differs from the DAOS sweeps, where the loss was in the first 0.5 s ([section 9](2026-10-21-e2e-lb-sweep.md#9-lb-rate-sweep-2045-gbps-2026-10-21)).
- **From 40 Gbps up, most loss is start-up loss** (event ids below ~100) on both paths.
  The rate stays at 0.01–0.06% per run, except one LB recvmmsg run at 60 Gbps with 266 events (0.44%), spread over the run.
- **recvmmsg has no large loss through the LB.** The 1.4–2.4% runs node-to-node at 40, 80 and 90 Gbps didn't happen through the LB.
- **The sender reaches its rate through the LB**: 78–80 Gbps at 80, and 89.5–89.9 at 90. Node-to-node it topped out at 75–76 and 87–88.
- **The node-to-node *crash* runs were probably receiver segfaults.** ebpf2203's `dmesg` has `e2sar_perf` segfaults (`error 6`) from 2026-10-26 at about the time of those runs.
  The sender then reported `Connection refused`, from ICMP port-unreachable replies.
  Through the LB, ICMP replies don't reach the sender, so a receiver that dies shows up only as an empty receiver log.
