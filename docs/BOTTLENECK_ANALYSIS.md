# Bottleneck analysis: e2sar_ft → e2sar_daos_recv → DAOS

Where the e2sar_ft → e2sar_daos_recv → DAOS path is limited on the ebpf testbed, measured on 2026-10-19 with PR #11 (`feature/daos-kv-recv-apps`), E2SAR 0.4.0rc1 and DAOS 2.6.5 (pool `sc26`).
For how to run the tests, see [EBPF_TESTBED_NOTES.md](EBPF_TESTBED_NOTES.md).

## Summary

| Part | Verdict | Evidence |
|---|---|---|
| Sender, per event | **About 10–12 Gbps per event** | One event is sent by one thread. No sender option changed this (tables 2–5) |
| Sender, multi-thread Segmenter | Scales with the number of files queued | One `e2sar_ft` with 4 / 8 / 16 files sent 43 / 70 / 58 Gbps (table 6) |
| Receiver, one port | **Limits to about 10–12 Gbps** | One `e2sar_ft` sends everything to one port and one receive thread. At 43+ Gbps the kernel dropped 40–76% of packets and lost every event (table 6) |
| Receiver socket buffer | Caused the single-port losses at ~10 Gbps until raised to 1 GiB | With 256 MiB: `RcvbufErrors` in 3 of 11 runs. With 1 GiB: 0 in 20 runs |
| Receiver, parallel ports | **Reliably lossless only up to ~25 Gbps** | N `e2sar_ft` processes to N ports, `--threads N`: 2–3 senders (20–26 Gbps) clean every time. 4 senders (~48 Gbps) clean in 1 of 6 runs. 8+ never (tables 7–8) |
| NIC receive ring | Part of the loss, not the root cause | At ring 1024, 4–8 senders lost frames to `rx_out_of_buffer`. At 8192 those drops mostly went away, but events were still lost (table 8) |
| **Receiving NIC port, no end-to-end flow control** | **The remaining limit at 4+ senders** | Every failed run in table 8 had 11–1,229 `rx_discards_phy` drops. ebpf2203 has sent pause frames, but ebpf2201 has received none: the switch doesn't push back to the sender. At ~1 frame in 27,000 lost, a 240k-frame event almost never survives |
| Receive-thread CPU placement | Not the cause | Pinning receive threads to CPUs 64+, away from the NIC's interrupt cores, changed nothing (table 8) |
| Through the ESnet LB (one `e2sar_ft`, N files, `--threads 16–32`) | **Same ~25 Gbps ceiling** | The LB spreads events over the worker's ports by random entropy, so the multi-thread Segmenter works without N processes. 2 events (11.5 Gbps) clean in 3 of 3 runs. 3 events (25.8 Gbps) clean in 2 of 4, and every failure had `rx_discards_phy` > 0. See [ESNET_LB_TESTING.md](ESNET_LB_TESTING.md#11-highest-lossless-throughput) |
| NFS source read | **2.2 Gbps when not cached** | Copying 33 GB uncached took 124 s. Stage the source on `/scratch` |
| Network | Not the bottleneck | 100 Gb/s link. 88 Gbps went out with no sender errors |
| DAOS KV writes | **Not the bottleneck up to at least ~48 Gbps** | `Lost in enqueue` was 0 in every run. Four ~2.1 GiB events (8.6 GB) committed and read back with matching digests (table 7) |
| DAOS dequeue threads (`--deq`) | **Limit with many smaller events** | With 95 KB synthetic EVIO events, 4 dequeue threads capped the receiver at ~24 Gbps while each DAOS write still took 0.3 ms. 16 threads reached 40 Gbps, 32 reached 61. Now the default is 16 ([E2E_BENCHMARK.md](E2E_BENCHMARK.md#3-dequeue-threads-the-hidden-bottleneck)) |
| Reassembler, events per second | **~85k events/s** | The same ceiling for 424 B and 95 KB events, and with 32 or 64 dequeue threads ([E2E_BENCHMARK.md](E2E_BENCHMARK.md)) |

## Setup

- **Path:** ebpf2201 (sender) → ebpf2203 (receiver and DAOS), both 100 Gb/s mlx5 NICs with MTU 9000 on the same /26 subnet. Each node has 128 CPUs on 2 NUMA nodes, and the NIC is on node 0.
- **Payload:** for tables 1–5, `frames_thread0_file0020.evio` (1,776,313,996 bytes) and a 3.5 MB random file. For tables 6–7, up to 16 files of about 2.1 GiB each (`frames_thread0_file0000`–`0015.evio`). `e2sar_ft` sends each file as one event.
- **Receiver:** `e2sar_daos_recv --timeout 10000`, 1 MiB KV chunks. Tables 1–5 use 1 dequeue thread with 64 in-flight puts. Tables 6–7 use 4 × 128.
- **Cleanup:** after each run the receiver's restored files and the DAOS container `e2sar-test` were deleted, and after each sweep the sender's `/scratch` source copies.
- **Metrics:** see [Throughput metrics](#throughput-metrics) below.

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

## Measurements

### 1. MTU

| Sender options | Sender goodput |
|---|---|
| `--mtu 1500 --rate 5` | 2.1 Gbps |
| `--mtu 9000 --rate 5` | 7.3–11.5 Gbps |
| `--mtu 1500 --rate 3 --smooth` | 0.67 Gbps |
| `--mtu 9000 --rate 3 --smooth` | 0.74 Gbps |

At MTU 1500 the sender sends 1.24 M frames instead of 199 k. Use MTU 9000.
`--smooth` pauses after every frame and stays below 1 Gbps at either MTU. A 1.78 GB event then takes about 20 s and always misses the 10 s reassembly timeout.

### 2. `--rate` sweep: source on NFS, receiver `-b 256 MiB`, MTU 9000

| `--rate` | Sender goodput | Committed | `RcvbufErrors` | Ingest | DAOS commit |
|---|---|---|---|---|---|
| 3 | 7.5 Gbps | 2 / 2 | 0 | 10.1 Gbps | 9.2 Gbps |
| 5 | 11.5 Gbps | 2 / 2 | 0 | 15.6 Gbps | 13.7 Gbps |
| 10 | 10.6 Gbps | 2 / 2 | 0 | 12.4 Gbps | 11.3 Gbps |
| 20 | 10.5 Gbps | 1 / 2 | 10,366 | – | – |
| 40 | 10.4 Gbps | 1 / 2 | 13,677 | – | – |
| −1 (unlimited) | 10.0 Gbps | 2 / 2 | 0 | 10.5 Gbps | 9.7 Gbps |

### 3. Same sweep, source on local `/scratch`

| `--rate` | Sender goodput | Committed | `RcvbufErrors` | Ingest | DAOS commit |
|---|---|---|---|---|---|
| 5 | 10.0 Gbps | 2 / 2 | 0 | 13.0 Gbps | 11.6 Gbps |
| 10 | 12.5 Gbps | 1 / 2 | 37,565 | – | – |
| 20 | 9.9 Gbps | 2 / 2 | 0 | 11.0 Gbps | 9.9 Gbps |
| −1 | 11.3 Gbps | 2 / 2 | 0 | 9.4 Gbps | 8.7 Gbps |

Sender goodput is the same as in table 2. However, table 2's source was most likely in ebpf2201's memory cache after repeated runs.
Copying 16 uncached files (33 GB) from `~/evio-data` (NFS) to `/scratch` took 124 s, which is **2.2 Gbps**. A sender reading uncached data over NFS would be limited to about that, so stage the source on `/scratch` first.
Losses with a 256 MiB buffer don't follow `--rate`: rate 10 lost here but not from NFS, and rate 20 lost from NFS but not here. They are random overflows of the receive buffer.

### 4. Sender and receiver options: source on `/scratch`, `--mtu 9000 --rate -1`, receiver `-b 1 GiB`, 2 runs each

| Setup | Sender goodput | Ingest | DAOS commit | `RcvbufErrors` |
|---|---|---|---|---|
| A: baseline | 11.2 / 11.2 Gbps | 11.7 / 11.6 Gbps | 10.7 / 10.7 Gbps | 0 / 0 |
| B: `-o sendmmsg` | 10.3 / 10.3 Gbps | 10.9 / 7.9 Gbps | 10.0 / 7.4 Gbps | 0 / 0 |
| C: `--sockets 16` | 11.2 / 10.8 Gbps | 11.7 / 11.2 Gbps | 10.8 / 10.3 Gbps | 0 / 0 |
| D3: `-o sendmmsg -b 64 MiB --cores 8–23` (16 threads); receiver `--cores 8` | 10.5 / 10.5 Gbps | 11.0 / 11.0 Gbps | 10.0 / 10.0 Gbps | 0 / 0 |
| T4: receiver `--threads 4` | 9.9 / 11.2 Gbps | 7.9 / 11.7 Gbps | 7.5 / 10.7 Gbps | 0 / 0 |

No sender option moved goodput out of the 10–11 Gbps band. With a 1 GiB receive buffer, none of these runs dropped a packet.

### 5. All directions at MTU 9000: `--rate 5`, receiver `-b 1 GiB`, source on NFS

| Run | Sender goodput | Ingest | DAOS commit | Lost |
|---|---|---|---|---|
| Loopback on ebpf2203 | 4.2 / 7.6 Gbps | 4.7 / 9.2 Gbps | 4.5 / 8.5 Gbps | 0 / 0 |
| ebpf2201 → ebpf2203 | 7.4 / 7.3 Gbps | 8.9 / 8.8 Gbps | 8.3 / 8.2 Gbps | 0 / 0 |
| ebpf2203 → ebpf2201 | 14.3 / 11.1 Gbps | 20.9 / 9.0 Gbps | 17.8 / 8.4 Gbps | 0 / 0 |

All 6 runs restored files with matching digests. The single 14.3 Gbps run shows the sender can sometimes go faster, but the spread between runs is large.

### 6. Multi-thread Segmenter: several files in one e2sar_ft

Source: N files of about 2.1 GiB from `~/evio-data`, staged on ebpf2201's `/scratch`. Sender: `--mtu 9000 --rate -1 --sockets N`. Receiver: 1 thread, `-b 1 GiB --deq 4 --inflight 128`.
`--rate -1` matters here. Any rate limit adds a gap between events and makes the threads send one after another.

| Files / send threads | Sender goodput | Wall time | Committed | Kernel `RcvbufErrors` |
|---|---|---|---|---|
| 1 / 4 | 9.6 Gbps | 4.9 s | 1 / 1 (digest PASS) | 0 |
| 4 / 4 | **43.3 Gbps** | 4.7 s | 0 / 4 | 384,314 (~40% of frames) |
| 8 / 8 | **70.1 Gbps** | 5.1 s | 0 / 8 | 1,400,896 (~73%) |
| 16 / 16 | 57.9 Gbps | 7.9 s | 0 / 16 | 2,923,834 (~76%) |

The multi-thread Segmenter works: each queued event gets its own send thread, and goodput scales almost linearly up to 8 threads. It peaks around 70 Gbps; 16 threads were slower than 8.
Every frame still goes to the URI's single port, though, so one receive thread has to take 43–70 Gbps. The socket overflows and every event is lost.

### 7. Parallel senders to parallel receive ports

Source: N files staged on ebpf2201's `/scratch`. Sender: N `e2sar_ft` processes started together, process *i* sending one file to port 20480+*i* with `--dataid 4321+i`, `--mtu 9000 --rate -1 --sockets 1`.
Receiver: `--threads N` (ports 20480 to 20480+N−1), `-b 1 GiB --deq 4 --inflight 128`. Each run's events were read back from DAOS and their digests checked, then the container was destroyed.

| Senders / threads | Each sender | Sum | Committed | Kernel drops | NIC `rx_out_of_buffer` | Digests |
|---|---|---|---|---|---|---|
| 2 / 2 | 9.2, 10.7 Gbps | 19.9 Gbps | 2 / 2 | 0 | not measured | PASS |
| 2 / 2 | 12.3, 12.9 Gbps | 25.2 Gbps | 2 / 2 | 0 | 0 | PASS |
| 3 / 3 | 9.1, 9.8, 7.4 Gbps | 26.3 Gbps | 3 / 3 | 0 | 0 | PASS |
| 4 / 4 | 11.4–12.3 Gbps | **48.2 Gbps** | 4 / 4 | 0 | 0 | PASS |
| 4 / 4 | 11.0–12.8 Gbps | 48.4 Gbps | 0 / 4 | 0 | not measured | – |
| 8 / 8 | 9.2–11.7 Gbps | 88.1 Gbps | 0 / 8 | 0 | not measured | – |

"Sum" adds up each process's own goodput estimate. The processes run at the same time, so it approximates the aggregate rate.
The receiver's *ingest* and *DAOS commit* figures are not shown. With N events arriving together, the window from first to last event is very short, and those rates come out too high (50–79 Gbps).

The two failed runs had 0 kernel UDP drops. Afterwards, the NIC counters on ebpf2203 showed `rx_out_of_buffer: 319,525` and `rx_discards_phy: 14,560` (cumulative), and the RX ring was 1024 of a possible 8192.
Later runs that recorded the counters before and after showed 0 NIC drops for 2–4 senders.

### 8. Parallel senders with RX ring 8192 and pinned receive threads

Same setup as table 7, but with the RX ring raised from 1024 to 8192 on both nodes (`ethtool -G enp1s0f1np1 rx 8192`). NIC and kernel counters were taken before and after every run.
*RX queues* shows how many receive queues carried a full file (~0.24 M frames). "2 flows on 1 queue" means two senders' flows hashed to the same queue.

| Senders | Receiver | Committed | `rx_out_of_buffer` | `rx_discards_phy` | `RcvbufErrors` | RX queues |
|---|---|---|---|---|---|---|
| 4 | `--threads 4` | 0 / 4 | 132,951 | 961 | 0 | not recorded |
| 4 | `--threads 4` | 0 / 4 | 0 | 11 | 0 | not recorded |
| 8 | `--threads 8` | 0 / 8 | 183,193 | 340 | 0 | 8 queues; 4 of them short by 30–60 k frames |
| 8 | `--threads 8` | 0 / 8 | 0 | 192 | 0 | all full; 2 flows on 1 queue |
| 12 | `--threads 12` | 0 / 12 | 0 | 231 | 181,561 | all full; 2 queues with 2 flows each |
| 16 | `--threads 16` | 0 / 16 | 0 | 1,229 | 0 | all full; 2 flows on 1 queue |
| 4 | `--cores 64-67` | 0 / 4 | 0 | 36 | 0 | all full |
| 4 | `--cores 64-67` | 0 / 4 | 0 | 62 | 0 | all full |
| 8 | `--cores 64-71` | 0 / 8 | 85,900 | 240 | 0 | the queue with 2 flows short by ~90 k frames |
| 8 | `--cores 64-71` | 0 / 8 | 0 | 426 | 0 | all full |

Sum of sender goodputs, in table order: 49.8, 46.0, 92.2, 90.8, 94.9, 96.3, 35.3, 50.8, 89.8 and 94.5 Gbps. 12 and 16 senders saturate the 100 Gb/s link. No sender reported errors.
Pause frames, read once after these runs: ebpf2203 had sent 820 since boot (`tx_pause_ctrl_phy`), and ebpf2201 had received 0 (`rx_pause_ctrl_phy`).

## Analysis

### Sender: one event is sent by one thread

In `src/e2sarDPSegmenter.cpp` (`Segmenter::SendThreadState::_threadBody`, around lines 403–426), the segmenter takes each event off its queue, picks one send socket round-robin (`roundRobinIndex`), and posts the **whole event** to the thread pool. That one worker calls `_send()` for every frame of the event.
`--rate` only adds a busy-wait **between** events (`interEventSleepUsec`). Frames within an event go out as fast as one thread can call `sendmsg`.

The test sends one 1.78 GB event, so:
- One thread on one core sends all 199 k frames. At 9000 bytes per frame, 10–11 Gbps is about 140–150 k frames/s, a typical single-core UDP send rate.
- `--sockets 16`, `--cores` and `-o sendmmsg` add or change threads that this event never uses.
- `--rate` makes no difference above about 5. Each event already goes out at the single-thread maximum, and with one large event there is almost no gap between events to throttle.
- Bursts within an event run at the single-thread maximum whatever `--rate` says. That's why the receiver overflowed at random rather than at a particular rate.

With several events queued, the same mechanism is what makes the multi-thread Segmenter scale (table 6). Each event goes to the next worker, and N events keep N workers busy at about 10–12 Gbps each.

### Receiver: one port per thread, then the NIC

Each E2SAR receive thread owns its ports. One `e2sar_ft` sends everything to one port, so one receive thread has to take everything, whether the sender uses 1 thread or 16.
- At about 10 Gbps, the occasional `RcvbufErrors` were bursts the socket couldn't absorb. A 1 GiB buffer (`-b 1073741824`; `net.core.rmem_max` is 2 GiB) fixed that, with no losses in 20 runs.
- At 43–70 Gbps into one port (table 6), no buffer helps. One thread can't drain the socket fast enough.

`--threads N` only helps when traffic arrives on N ports. `e2sar_ft` has no switch for E2SAR's `multiPort` flag. Running N `e2sar_ft` processes, one per port with distinct `--dataid`s, achieves the same thing (table 7). That is reliably lossless for 2–3 senders (~25 Gbps).

From 4 senders on, frames are lost in the NIC, before the kernel sees them:
- **`rx_out_of_buffer`**: a receive queue ran out of host buffers. At ring 1024 this happened often. At 8192 it happened only in some runs, mostly on a queue that had two flows hashed to it (~22 Gbps for one core).
- **`rx_discards_phy`**: the NIC port's buffer overflowed. This happened in **every** failed run, even with the ring at 8192, receive threads pinned off the interrupt cores, and all flows on separate queues. The NIC responded with pause frames, but the switch didn't pass them back to the sender, so the extra frames were dropped rather than slowed.

Each failed run lost only 11–1,229 frames out of about 1–4 M, roughly 1 frame in 27,000 at 4 senders. But `e2sar_ft` sends each file as one event of ~240 k frames, with no retransmission. At that loss rate the chance of an event arriving intact is about e^−9, effectively zero.
So the limit at 4+ senders is not an E2SAR or DAOS bottleneck. It's a lossy UDP path combined with events too large to survive any loss.

### DAOS: keeps up

`Lost in enqueue` was 0 in every run, including 4 events (8.6 GB) arriving together at about 48 Gbps. All were committed and read back with matching digests.
In the single-sender runs, commit ran at 88–96% of ingest, and `daos_kv_put` p99 latency was tens of ms. DAOS was never the limiting stage.
Its ceiling is still unknown. A clean sustained rate needs a longer run (tens of files per port) and a rate measured from the first packet, not the first completed event.

## Not yet tested

1. **End-to-end flow control.** Have the network team check whether the switch port toward the sender honors and propagates pause frames (802.3x), or set up PFC. With flow control end to end, `rx_discards_phy` should become backpressure instead of loss.
2. **Smaller events.** Split the files (for example with `evio_split.py`) into events of tens of MB. At the measured loss rate, a 10 MB event (~1,100 frames) survives about 96% of the time, versus about 0% for a 2 GiB event. That shows how much of the problem is event size.
3. **Rate-limited parallel senders.** Cap each sender, for example 4 × 6 Gbps, to find the highest lossless aggregate. `--rate` only spaces out events, and `--smooth` is too slow, so this needs many smaller events per sender.
4. **Sustained parallel load.** Send several files per sender process (N × M files) to measure steady DAOS commit throughput, not single-burst numbers. Add `--timing-csv` on the receiver.
5. **A `multiPort` switch in `e2sar_ft`.** One process with `--sockets N` sending to N ports would replace the N-process workaround.
6. **Variation between runs.** The same settings varied by up to 2×, for example 9.0 versus 20.9 Gbps ingest. More repeats, and pinning the receive threads to NIC-local cores (NUMA node 0, CPUs 0–63), would show whether that comes from the nodes or the network.
