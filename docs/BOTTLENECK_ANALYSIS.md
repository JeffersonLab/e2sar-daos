# Bottleneck analysis: e2sar_ft → e2sar_daos_recv → DAOS

Where the e2sar_ft → e2sar_daos_recv → DAOS path is limited on the ebpf testbed, measured on 2026-10-19 with PR #11 (`feature/daos-kv-recv-apps`), E2SAR 0.4.0rc1 and DAOS 2.6.5 (pool `sc26`).
For how to run the tests, see [EBPF_TESTBED_NOTES.md](EBPF_TESTBED_NOTES.md).

## Summary

| Part | Verdict | Evidence |
|---|---|---|
| Sender (`e2sar_ft`) | **Limits throughput at about 10–11 Gbps for a single large file** | One event is sent by one thread. No sender option changed this |
| Receiver socket buffer | **Caused all the losses, until raised to 1 GiB** | With 256 MiB: `RcvbufErrors` in 3 of 11 runs. With 1 GiB: 0 in 20 runs |
| Receiver threads | Not a factor with `e2sar_ft` | `--threads 4` opens 4 ports, but `e2sar_ft` sends to only one |
| NFS source read | **Would be the bottleneck if the file is not cached** | Uncached NFS reads ran at about 2.2 Gbps. The "NFS" runs here read `file0020` from ebpf2201's memory cache, so they matched `/scratch`. Send from `/scratch` |
| Network | Not the bottleneck | A 100 Gb/s link, used at about 10% |
| DAOS KV writes | Not the bottleneck | Commit ran at about 90% of ingest in every run, `Lost in enqueue` was always 0, and up to 17.8 Gbps was committed |

## Setup

- **Path:** ebpf2201 (sender) → ebpf2203 (receiver and DAOS), both 100 Gb/s mlx5 NICs with MTU 9000 on the same /26 subnet. Each node has 128 CPUs on 2 NUMA nodes, and the NIC is on node 0.
- **Payload:** `frames_thread0_file0020.evio` (1,776,313,996 bytes) and a 3.5 MB random file. `e2sar_ft` sends each file as one event.
- **Receiver:** `e2sar_daos_recv --timeout 10000`, 1 MiB KV chunks, 1 dequeue thread with 64 in-flight puts.
- **Metrics:**
  - *sender goodput*: `e2sar_ft`'s own estimate for the whole run.
  - *ingest*: the receiver's rate from the first event received to the last.
  - *DAOS commit*: the rate from the first event received to the last event committed.
  - *`RcvbufErrors`*: the kernel UDP counter on the receiver, taken from `/proc/net/snmp` before and after each run.

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

## Analysis

### Sender: one event is sent by one thread

In `src/e2sarDPSegmenter.cpp` (`Segmenter::SendThreadState::_threadBody`, around lines 403–426), the segmenter takes each event off its queue, picks one send socket round-robin (`roundRobinIndex`), and posts the **whole event** to the thread pool. That one worker calls `_send()` for every frame of the event.
`--rate` only adds a busy-wait **between** events (`interEventSleepUsec`). Frames within an event go out as fast as one thread can call `sendmsg`.

The test sends one 1.78 GB event, so:
- One thread on one core sends all 199 k frames. At 9000 bytes per frame, 10–11 Gbps is about 140–150 k frames/s, a typical single-core UDP send rate.
- `--sockets 16`, `--cores` and `-o sendmmsg` add or change threads that this event never uses.
- `--rate` makes no difference above about 5. Each event already goes out at the single-thread maximum, and with one large event there is almost no gap between events to throttle.
- Bursts within an event run at the single-thread maximum whatever `--rate` says. That's why the receiver overflowed at random rather than at a particular rate.

### Receiver: socket buffer, not threads

All losses showed up as kernel `RcvbufErrors`: the one receive thread didn't always empty its socket in time during a burst. A 1 GiB buffer (`-b 1073741824`; `net.core.rmem_max` is 2 GiB) absorbs those bursts. There were no losses in all 20 later runs.

`--threads 4` opened ports 20480–20483, but `e2sar_ft` has no multi-port option, and without a load balancer every frame goes to the URI's single port. The extra threads sat idle. Results were the same as with one thread.

### DAOS: keeps up

In every clean run, DAOS commit ran at 88–96% of ingest, `Lost in enqueue` was 0, and `daos_kv_put` p99 latency was tens of ms. The highest commit was 17.8 Gbps, in the ebpf2203 → ebpf2201 run. DAOS was never the limiting stage. Its own ceiling isn't measured yet, because the sender saturates first.

## Not yet tested

1. **Many files at once.** Several events in the queue would keep several send threads busy. For example, send 8–16 of the 2 GiB files from `~/evio-data` with `--sockets 8` or `16`, and expect goodput to scale until the receiver or DAOS limits it. That would also measure the DAOS ceiling. The receiver may then need `--deq 2 --inflight 128`.
2. **Multiple receive threads.** These need a sender that spreads frames over ports, which E2SAR's `multiPort` segmenter flag does, or a load balancer. `e2sar_ft` would need a command-line switch for `multiPort`.
3. **Variation between runs.** The same settings varied by up to 2×, for example 9.0 versus 20.9 Gbps ingest. More repeats, and pinning the receiver to NIC-local cores, would show whether that comes from the nodes or the network.
