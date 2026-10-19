# End-to-end benchmark: synthetic EVIO frames → E2SAR → DAOS

How fast synthetic EVIO streaming time frames go from a sender on **ebpf2201**, through E2SAR (back-to-back or through the ESnet EJFAT load balancer), to `e2sar_daos_recv` on **ebpf2203** and into DAOS, without losing any. Also where each event's time goes on the way.
Measured on 2026-10-19 with E2SAR 0.4.0rc1, DAOS 2.6.5 (pool `sc26`) and LB 638 on `ejfat-lb.es.net`.

## Summary

| | Result |
|---|---|
| **Highest lossless E2E, through the LB** | **33.7 Gbps** E2E (35.3 Gbps received into DAOS). Lossless in 2 of 2 runs at 35 Gbps. Every run at 40 and 45 lost events |
| Highest lossless E2E, back-to-back | 24.9 Gbps E2E (25.2 received). Lossless in 1 of 2 runs at that rate |
| Lossless in every run | **20 Gbps through the LB (3 of 3)**, the new defaults. Back-to-back the same settings passed 1 of 3 |
| Highest with any loss | 61.2 Gbps received (59.8 E2E), back-to-back, 99.96% of events delivered |
| What loses events | Frames dropped at ebpf2203's NIC port (`rx_discards_phy`). The LB path smooths the bursts, so loss starts later through it than back-to-back. Never DAOS, never the sockets |
| What limits the rate | 1. The DAOS dequeue threads: 4 cap at ~24 Gbps, 16 reach 40, 32 reach 61. 2. Then the Reassembler at ~85k events/s, whatever the event size |
| Latency at the defaults | Queued at the sender → committed in DAOS: **0.3–0.5 ms p50** back-to-back, most of it the DAOS write. **75.6 ms p50** through the LB, almost all of it the WAN round trip to ESnet. See [latency per stage](#latency-per-stage-ms-same-runs) |
| iperf3 baseline | Not measured yet. The nightly iperf3 job (`~/daq/iperf3-nightly`) will give the zero-loss UDP rate between the nodes |

## The pieces

```
ebpf2201                                                         ebpf2203
e2sar_evio_send                                                  e2sar_daos_recv
  P Segmenters (dataId 4321+i), paced                              T receive threads (ports PORT..PORT+T-1)
  each event = EVIO v6 record of F time frames  ──UDP──▶ [LB] ──▶  Reassembler ──▶ D dequeue threads ──▶ DAOS KV
                                                                                  (own event queue, I puts each)
```

| Tool | What it does |
|---|---|
| [`bin/e2sar_evio_send`](../bin/e2sar_evio_send.cpp) | Builds synthetic EVIO v6 events and sends them with P E2SAR Segmenters, paced to a total rate. Writes per-event send times and the SHA-256 of every event it can send |
| [`bin/e2sar_daos_recv`](../bin/README.md#e2sar_daos_recv) | The existing receiver, unchanged except that it now prints its clock start so its per-event times can be matched with the sender's |
| [`bin/e2sar_e2e_bench.sh`](../bin/e2sar_e2e_bench.sh) | Runs one test: starts the receiver, waits for it (and the LB) to be ready, sends, waits for every commit, reads every event back from DAOS, analyzes, cleans up |
| [`bin/e2sar_e2e_analyze.py`](../bin/e2sar_e2e_analyze.py) | Joins the two hosts' per-event times, checks every digest, computes throughput and latency per stage |

You don't need a different receiver. `e2sar_daos_recv` stores any event, and the bench checks that what it stored is byte-for-byte what was sent.

### Synthetic EVIO frames

Each E2SAR event is one EVIO v6 record holding F frames (`-f`, default 256), with the layout of the CODA streaming data in `frames_thread0_*.evio` and `EC5_EC6_cosmics_*.evio`:

```
Record header (14 words, magic 0xc0da0100, bit info 0x80004206) + index array when F > 1
└─ Built time frame          bank 0xFF60, bank of banks            × F
   ├─ Stream info            bank 0xFF31: TSS 0x32 (frame, timestamp), AIS 0x42 (one word per ROC)
   └─ ROC 0x12, 0x13         bank of banks
      ├─ ROC stream info     bank 0xFF30: TSS 0x31, AIS 0x41 (port info)
      └─ Payload banks       tags 1, 3, …, 15; ~74% empty, as in the real data; hit words otherwise
```

- A frame is ~423 bytes on average (the cosmics data: 426). Frame n has timestamp n × 65,536 ns.
- 256 frames make a ~95 KB event, and 4096 frames ~1.5 MB.
- Each Segmenter sends events numbered 1…N. Event n is copy n mod 64 of 64 distinct events built at start, so every event's digest is known in advance. The bench reads **every** event back from DAOS and checks its digest.
- Each Segmenter has its own dataId (4321 + i), so DAOS keys never collide: `<prefix>/<evt>/<dataId>/<chunk>`.
- Events get random entropy, so through the LB they spread over the receiver's T ports. Back-to-back (no LB), Segmenter i sends to port PORT + i (`--port-per-segmenter`), so only P receive threads get traffic. The staged E2SAR 0.4.0rc1 has no `multiPort` sender option.
- `--rate` is the total over all Segmenters. Each producing thread paces its own share by bytes. The Segmenter's own rate limit works in whole microseconds per event, too coarse for small events.

## How to run

Prerequisites as in [ESNET_LB_TESTING.md](ESNET_LB_TESTING.md), steps 1–2: `~/.bashrc` with `E2SAR_BIN` and, for LB runs, `EJFAT_URI`, and e2sar-daos built in `~/daq/e2sar-daos`.
The nodes can't SSH to each other, so start the two roles in two terminals, one per node, with the **same TAG and mode**. They meet through files in the shared `$HOME/e2sar_run/bench/<TAG>/`.

```bash
# ebpf2203 (receiver), start first
bash ~/daq/e2sar-daos/bin/e2sar_e2e_bench.sh recv run1 -m lb
# ebpf2201 (sender)
bash ~/daq/e2sar-daos/bin/e2sar_e2e_bench.sh send run1 -m lb
```

`-m lb` goes through the LB in `$EJFAT_URI` (both sides `--withcp`). `-m b2b` sends straight to ebpf2203.
Both sides wait for each other: the sender until the receiver is listening (and, with the LB, has slots), the receiver until the sender is done and every event is committed (or 15 s without progress).
The receiver then prints the result:

```
run1 PASS  mode=lb P=16 s=1 f=256 rate=20.0 T=32 D=16x128 C=1024  committed 100000/100000, verified 100000
  received 19.97 Gbps (26238 evt/s, 95161 B/event)  e2e 19.80 Gbps  sender 19.97 Gbps
  latency sender   p50     0.070  p90     0.077  p99     0.100  max 2.1 ms
  latency network  p50    ...
  latency daos     p50     0.230  ...
  latency total    p50    ...
  drops: rx_discards_phy 0 rx_out_of_buffer 0 UdpRcvbufErrors 0
```

The same as one JSON line goes to `$RUN/bench/results.jsonl`. The exit status is 0 only for PASS.

| Option | Side | Default | |
|---|---|---|---|
| `-m lb\|b2b` | both | `lb` | Through the LB, or back-to-back |
| `-P` | send | 16 | Segmenters, each with its own producing thread and dataId |
| `-s` | send | 1 | Send sockets (threads) per Segmenter |
| `-n` | send | 6250 | Events per Segmenter (100,000 in total by default) |
| `-f` | send | 256 | EVIO frames per event (~95 KB) |
| `-r` | send | 20 | Total rate in Gbps (−1: unlimited) |
| `-T` | recv | 32 | Receive threads (ports). Back-to-back, use T ≥ P |
| `-D`, `-I` | recv | 16, 128 | Dequeue threads, and in-flight puts per thread |
| `-C` | recv | 1024 | DAOS chunk size in KiB (0: whole event in one value). KV sink only |
| `-S kv\|dfuse\|pil4dfs` | recv | `kv` | Where events go: the DAOS KV object (all results here), or files through dfuse. See [DFUSE_TESTING.md](DFUSE_TESTING.md) |

Environment overrides: `SEND_IP`, `RECV_IP`, `PORT` (20600), `POOL` (`sc26`), `CONT` (`e2sar-bench`), `NIC`, `RUN`.

- **Check that the nodes are idle first,** as in [ESNET_LB_TESTING.md step 4](ESNET_LB_TESTING.md). The receiver refuses to start if `PORT` is in use.
- **Clean-up is built in.** The receiver destroys the container after reading it back. There are no source files: the sender builds its events in memory. Each run leaves only its logs and CSVs in `$RUN/bench/<TAG>/`. For 100,000 events that's about 10 MB.
- **A run fails** when any event is missing or has the wrong digest. `rx_discards_phy` > 0 almost always comes with it.

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
- The "network" stage includes the wait for a dequeue thread. When the dequeue threads are the bottleneck, it's that wait that grows, not "daos". See [the dequeue thread sweep](#3-dequeue-threads-the-hidden-bottleneck).
- With an unpaced sender, events wait in the sender's queue, so "sender" and "total" mostly measure the backlog. Use a paced sender (`-r`) for latency.

## Results (back-to-back unless stated)

### 1. Event size, unpaced, 4 Segmenters, 4 dequeue threads

| Frames/event | Event | Received | Events/s | Committed | NIC discards |
|---|---|---|---|---|---|
| 1 | 424 B | 0.29 Gbps | 85,396 | 200,000 / 200,000 | 0 |
| 16 | 6.0 KB | 3.60 Gbps | 74,923 | 199,995 / 200,000 | 5 |
| 256 | 95 KB | 30.7 Gbps | 40,344 | 49,835 / 50,000 | 1,206 |
| 4096 | 1.5 MB | 40.6 Gbps | 3,335 | 4,873 / 5,000 | 2,201 |
| 16384 | 6.1 MB | 53.7 Gbps | 1,102 | 1,966 / 2,000 | 636 |

One frame per event, as the DAQ writes them, tops out at ~85k events/s, only 0.3 Gbps. Throughput needs many frames per event.
Unpaced, the sender reaches 77–94 Gbps and overruns ebpf2203's NIC port.

### 2. Pacing and burst size

| Setting | Rate | Received | Committed | NIC discards |
|---|---|---|---|---|
| 4 × 2 sockets, 1.5 MB events | 20 | 20.0 | 9,948 / 10,000 | 495 |
| same | 40 | 39.8 | 9,846 / 10,000 | 1,199 |
| 1 Segmenter × 1 socket, 1.5 MB, unpaced | (12.0) | 12.0 | **5,000 / 5,000** | 0 |
| 4 × 2 sockets, **95 KB** events | 10 | 10.0 | **100,000 / 100,000** | 0 |
| same | 20 | 19.6 | **100,000 / 100,000** | 0 |

Pacing alone doesn't stop the discards with 1.5 MB events. Each event leaves one send thread as a ~10 Gbps line-rate burst, and the bursts of several Segmenters overlap at the receiving port.
With 95 KB events the bursts are 16× shorter, and 20 Gbps is lossless.

### 3. Dequeue threads: the hidden bottleneck

16 Segmenters, 95 KB events:

| Dequeue threads | Rate | Received | network p50 | daos p50 | Committed | NIC discards |
|---|---|---|---|---|---|---|
| 4 | 30 | 30.1 | 27 ms | 0.28 ms | 99,993 / 100,000 | 33 |
| 4 | 40 | **24.3** | **694 ms** | 0.33 ms | 100,000 / 100,000 | 0 |
| 16 | 40 | **40.5** | **0.20 ms** | 0.22 ms | 99,981 / 100,000 | 92 |
| 32 | 50 | 51.0 | 0.29 ms | 0.24 ms | 99,946 / 100,000 | 311 |
| 32 | 60 | **61.2** | 0.15 ms | 0.33 ms | 99,960 / 100,000 | 281 |
| 64 | 60 | 62.0 | 181 ms | 0.86 ms | 99,988 / 100,000 | 96 |
| 64 | 80 | 65.3 | 335 ms | 1.02 ms | 99,972 / 100,000 | 148 |

- With 4 dequeue threads the receiver can't go past ~24 Gbps at this event size. The events wait up to a second in the Reassembler's queue, but each DAOS write still takes only 0.3 ms. The per-event "daos" latency looks fine while the dequeue stage is the bottleneck.
- 16 threads reach 40 Gbps, and 32 reach 61 Gbps.
- 64 threads don't help. The receiver stays at ~85k events/s, the same ceiling as for 424-byte events, so that limit is per event (the E2SAR Reassembler), not per byte or in DAOS. `--deq` is independent of `--threads`.
- That's why `e2sar_daos_recv` now defaults to `--deq 16 --inflight 128`.

### 4. Highest lossless rate

16 Segmenters × 1 socket, 95 KB events, 16 dequeue threads, 100,000 events per run.

Back-to-back:

| Rate | Runs | Lossless | Received | E2E | Lost events | NIC discards |
|---|---|---|---|---|---|---|
| 25 | 2 | 1 | 25.2 | 24.9 | 0, 4 | 0, 4 |
| 30 | 2 | 0 | 30.2 | 29.9 | 8, 26 | 50, 142 |
| 35 | 2 | 0 | 35.3 | 34.9 | 5, 14 | 15, 66 |

Through LB 638:

| Rate | Runs | Lossless | Received | E2E | Lost events | NIC discards |
|---|---|---|---|---|---|---|
| 20 | 3 | **3** | 20.0 | 19.6 | 0, 0, 0 | 0, 0, 0 |
| 25 | 2 | 1 | 25.1 | 24.3 | 0, 17 | 0, 75 |
| 30 | 2 | 1 | 30.2 | 29.1 | 0, 10 | 0, 52 |
| **35** | 2 | **2** | **35.3** | **33.7** | 0, 0 | 0, 0 |
| 40 | 2 | 0 | 40.4 | 38.3 | 11, 177 | 35, 1,127 |
| 45 | 2 | 0 | 45.5 | 42.9 | 5, 7 | 6, 51 |

Loss doesn't rise smoothly with the rate. Between 25 and 35 Gbps a run either has no discards at all or a short burst of them. With 2–3 runs per rate, the numbers give a rough idea of how often that happens, not a sharp threshold. From 40 Gbps up, every run lost events.

Every lost event, in both modes, came with NIC port discards on ebpf2203. There were no socket drops (`UdpRcvbufErrors` 0), no ring overflows (`rx_out_of_buffer` 0), no reassembly or enqueue loss, and no failed DAOS writes.

### 5. Confirmation runs with the defaults

16 Segmenters × 1 socket, 95 KB events, 20 Gbps, 32 receive threads, 16 × 128 dequeue, 1 MiB chunks, 100,000 events (9.5 GB) per run, alternating modes:

| Mode | Run | Result | Received | E2E | Lost events | NIC discards |
|---|---|---|---|---|---|---|
| back-to-back | 1 | FAIL | 20.09 | 19.95 | 2 | 2 |
| back-to-back | 2 | FAIL | 20.09 | 19.95 | 15 | 25 |
| back-to-back | 3 | **PASS** | 20.09 | 19.95 | 0 | 0 |
| **LB** | 1 | **PASS** | 20.07 | 19.58 | 0 | 0 |
| **LB** | 2 | **PASS** | 20.05 | 19.57 | 0 | 0 |
| **LB** | 3 | **PASS** | 20.03 | 19.57 | 0 | 0 |

**Through the LB, 20 Gbps was lossless every time, with 0 NIC discards. Back-to-back, the same traffic lost a few events in 2 of 3 runs.**
The LB path (ESnet WAN and the LB's FPGA data plane) evidently smooths the line-rate bursts of each event before they reach ebpf2203. Back-to-back they arrive straight from the sender's NIC.
E2E is 0.4 Gbps lower through the LB only because of the extra ~75 ms before the first event lands.

#### Latency per stage (ms), same runs

| Mode | Stage | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| back-to-back | sender | 0.07–0.10 | 0.09–0.13 | 0.14–0.17 | 2–3 |
| | network | 0.02–0.11 | 0.07–0.19 | 54–58 | 66–68 |
| | daos | 0.22–0.24 | 0.26–0.33 | 0.9–1.3 | 45–46 |
| | **total** | **0.32–0.46** | **0.40–0.62** | **56–61** | 82–88 |
| LB | sender | 0.09–0.10 | 0.11–0.13 | 0.14–0.18 | 1.7–3.0 |
| | network | 75.2–75.3 | 75.5 | 122–130 | 139–144 |
| | daos | 0.22–0.23 | 0.26 | 0.85–1.18 | 44–47 |
| | **total** | **75.5–75.6** | **75.8–75.9** | **126–133** | 155–158 |

- **Back-to-back, most of an event's 0.3–0.5 ms goes to DAOS** (0.22 ms for one 95 KB put plus the metadata put). The sender needs ~0.1 ms, and the wire plus reassembly is within the ±0.2 ms clock error. Values that small (and the occasional slightly negative one) are clock noise.
- **Through the LB, the WAN dominates: ~75 ms**, the round trip from JLab to the ESnet LB and back (the path's RTT is 71 ms). Everything else is the same as back-to-back.
- **The p99 tail of ~55 ms (back-to-back) or ~50 ms over the WAN** is in the "network" stage, so events then wait for a dequeue thread. The "daos" stage has its own tail of ~45 ms at the very end (max). Both look like occasional DAOS stalls of tens of milliseconds, which hold up a dequeue thread and the events queued behind it.

### 6. DAOS layout: chunks, or one value per event

`e2sar_daos_recv` stores each event in one KV object as `<prefix>/<evt>/<dataId>/<chunk>` values of `--chunk` KiB, plus a metadata key written last.
For events up to 1 MiB, including the default 95 KB, "by chunk" and "by dataId" are the same thing: one data put and one metadata put per event.
They differ only for bigger events. Compared with 1.5 MB events (4096 frames, 16 dequeue threads, 10,000 events), 1 MiB chunks (2 puts in parallel) vs `--chunk 0` (the whole event as one value):

| Rate | Chunk | Received | daos p50 | daos p90 | daos p99 | Committed | NIC discards |
|---|---|---|---|---|---|---|---|
| 20 | 1 MiB | 19.43 | **0.57** | **1.32** | 22.4 | 9,673 | 4,215 |
| 20 | whole event | 19.72 | 1.15 | 1.69 | 26.3 | 9,813 | 1,187 |
| 40 | 1 MiB | 40.43 | **0.55** | **0.71** | 22.7 | 9,985 | 267 |
| 40 | whole event | 40.34 | 0.66 | 0.87 | 24.3 | 9,979 | 121 |

- **Throughput: no difference.** DAOS isn't the limit at either rate. The lost events all come from NIC discards, which 1.5 MB line-rate bursts cause back-to-back regardless of the DAOS layout.
- **Latency: 1 MiB chunks are slightly faster** (0.55–0.57 vs 0.66–1.15 ms p50), because one event's chunks are written in parallel.
- So keep `--chunk 1024`. One value per event saves nothing measurable and gives up the parallel writes for large events. One KV object for everything was not a limit either: at 61 Gbps each write still took 0.33 ms p50. Splitting into one object per event or per dataId (with dataIds as keys) would change the read-back layout without solving a measured problem. It might be worth testing if many receivers write at once.

## Why the lossless ceiling is ~25 Gbps back-to-back and ~35 through the LB

- **NIC port discards on ebpf2203** (`rx_discards_phy`), as in [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md). The switch doesn't pass ebpf2203's pause frames back to the sender, so short overloads drop frames, and one lost frame loses its whole event.
- **Bursts, not the average rate.** Each event leaves the sender as a line-rate burst. Back-to-back, those bursts reach ebpf2203 as they are. Through the LB (ESnet WAN and the LB's data plane) they arrive smoothed, so the same average rate drops fewer frames: 20 Gbps passed 3 of 3 through the LB but 1 of 3 back-to-back.
- **Possibly made worse by DAOS on the same adapter.** E2SAR arrives on `enp1s0f1np1` (Ethernet, 100 Gb/s) and DAOS writes go out on `ibp1s0f0` (InfiniBand HDR, 200 Gb/s). They're the two ports of **one ConnectX-6** (PCI `01:00.0` and `01:00.1`, PCIe Gen4 x16, NUMA node 0), so they share its host interface and on-card buffers.
  The one paced 40 Gbps run that had 0 discards wrote to DAOS at only 24 Gbps (4 dequeue threads). At 25 Gbps and more written to DAOS, there were nearly always some. This is a correlation, not a proof.
- **Not the cause:** DAOS (writes take 0.2–0.5 ms p50 at every rate, no failed puts), socket buffers (1 GiB, no `RcvbufErrors`), the RX ring (8192, no `rx_out_of_buffer`).

To go higher without loss: flow control or a deeper buffer at ebpf2203's switch port, pacing inside each event (the staged E2SAR's `--smooth` is too slow, below 1 Gbps), DAOS on a separate adapter (worth testing), or several receiver nodes behind the LB.

An iperf3 zero-loss UDP rate between the nodes would show how much of this is the network path alone. It hasn't been measured yet. The nightly iperf3 job (`~/daq/iperf3-nightly`, binary search for the zero-loss rate with 8972-byte datagrams) will provide one.

## Recommended settings

These are now the defaults of `e2sar_evio_send`, `e2sar_e2e_bench.sh` and (`--deq`, `--inflight`) `e2sar_daos_recv`:

| | Setting | Why |
|---|---|---|
| Sender | 16 Segmenters × 1 socket | Spreads events over many receive threads (back-to-back: ports PORT…PORT+15). One socket each keeps bursts short |
| Sender | 256 frames per event (~95 KB) | Short line-rate bursts, but few enough events to stay far below the receiver's ~85k events/s |
| Sender | `--rate 20` | The only rate with no loss in every run through the LB (3 of 3). Through the LB, up to 35 was lossless in some runs. Back-to-back, no rate tested was lossless every time |
| Receiver | `--threads 32` | Through the LB: more ports, fewer events sharing one. Back-to-back: at least P |
| Receiver | `--deq 16 --inflight 128` | 4 dequeue threads cap at ~24 Gbps. 16 are enough to 40 Gbps |
| Receiver | `-b 1073741824 --timeout 10000` | As before. No socket drops |
| Receiver | `--chunk 1024` | As fast as one value per event, a little faster for events > 1 MiB. See [DAOS layout](#6-daos-layout-chunks-or-one-value-per-event) |
