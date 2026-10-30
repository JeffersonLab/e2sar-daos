---
type: How-To
title: "End-to-end benchmark: EVIO frames → E2SAR → DAOS"
description: "Run scripts/e2sar_e2e_bench.sh: the tools, the events, the options, and the sender and receiver settings it defaults to."
tags: [bench, e2sar_evio_send, e2sar_e2e_bench, ebpf-testbed]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/E2E_BENCHMARK.md", title: "E2E_BENCHMARK.md at 0d85d00" }
---

# End-to-end benchmark: EVIO frames → E2SAR → DAOS

How to run the bench that measures how fast EVIO frames go from a sender on **ebpf2201**, node-to-node or through the ESnet EJFAT LB, into DAOS through `e2sar_daos_recv` on **ebpf2203**. The results are in [results/](../results/index.md), starting with the [summary](../results/e2e-summary.md); the metrics in [throughput-metrics](../concepts/throughput-metrics.md).

## The pieces

```
ebpf2201                                                         ebpf2203
e2sar_evio_send                                                  e2sar_daos_recv
  P Segmenters (dataId 4321+i), paced                              T receive threads (ports PORT..PORT+T-1)
  each event = EVIO v6 record of F file frames  ──UDP──▶ [LB] ──▶  Reassembler ──▶ D dequeue threads ──▶ DAOS KV
                                                                                  (own event queue, I puts each)
```

| Tool | What it does |
|---|---|
| [`bin/e2sar_evio_send`](../../bin/e2sar_evio_send.cpp) | Reads an EVIO v6 file and sends the frames in it, F per event, with P E2SAR Segmenters, paced to a total rate. Writes per-event send times and the SHA-256 of every event it can send |
| [`bin/e2sar_daos_recv`](../reference/e2sar_daos_recv.md#e2sar_daos_recv) | The existing receiver. It now prints its clock start so its per-event times can be matched with the sender's. `--synthetic` measures it without a sender ([section 7](../results/2026-10-20-synthetic-sink.md#8-the-sink-without-a-sender---synthetic)) |
| [`scripts/e2sar_e2e_bench.sh`](../../scripts/e2sar_e2e_bench.sh) | Runs one test: starts the receiver, waits for it (and the LB) to be ready, sends, waits for every commit, reads every event back from DAOS, analyzes, cleans up |
| [`bin/e2sar_e2e_analyze.py`](../../bin/e2sar_e2e_analyze.py) | Joins the two hosts' per-event times, checks every digest, computes throughput and latency per stage. `--profile S` shows latency and loss per S-second slice of the run |
| [`scripts/e2sar_e2e_sweep.sh`](../../scripts/e2sar_e2e_sweep.sh) | Runs a rate sweep of the bench from a machine that can SSH to both nodes |

You don't need a different receiver. `e2sar_daos_recv` stores any event, and the bench checks that what it stored is byte-for-byte what was sent.

### The events

`e2sar_evio_send` reads an EVIO v6 file (`-F`) into memory and sends it by frames: each record of the file is one frame (e.g. a built time frame of `frames_thread0_*.evio`). Each E2SAR event is `-f F` consecutive frames (`--frames-per-event`, default 1), their bytes unchanged and back to back, record headers included. The file header and trailer aren't sent.

- The event size is F times the frame size in the file. CODA streaming frames are small (~407–426 bytes), so with `-f 1` a file of them gives many small events: mind the Reassembler's ~85k events/s. `-f 50` gives ~20 KB events, the size coda-fb is expected to send, so 2.5 Gbps is ~15k events/s.
- The file's K frames make E = ⌈K / F⌉ distinct events; the last one has fewer frames when F doesn't divide K. The sender prints E as `Distinct events:`.
- Each Segmenter sends events numbered 1…N, the file's E distinct events in order. Event n is distinct event (n − 1) mod E, so a run with N > E starts over from the start of the file. `--digests` writes the SHA-256 of every distinct event (with `-f 1`: of every frame), so every event's digest is known in advance. The bench reads **every** event back from DAOS and checks its digest.
- Each Segmenter has its own dataId (4321 + i), so every event is a distinct `(evtNum, dataId)` in DAOS. See the [DAOS layout examples](../concepts/daos-layout.md#examples).
- Events get random entropy, so through the LB they spread over the receiver's T ports. Node-to-node (no LB), Segmenter i sends to port PORT + i (`--port-per-segmenter`), so only P receive threads get traffic. The staged E2SAR 0.4.0rc1 has no `multiPort` sender option.
- `--rate` is the total over all Segmenters. Each producing thread paces its own share by bytes. The Segmenter's own rate limit works in whole microseconds per event, too coarse for small events.

**The old synthetic frames.** The results on this page were measured before the sender read files. It built 64 distinct events per Segmenter in memory (event n was copy n mod 64), each an EVIO v6 record of F frames (`-f`, since removed), with the layout of the CODA streaming data in `frames_thread0_*.evio` and `EC5_EC6_cosmics_*.evio`:

```
Record header (14 words, magic 0xc0da0100, bit info 0x80004206) + index array when F > 1
└─ Built time frame          bank 0xFF60, bank of banks            × F
   ├─ Stream info            bank 0xFF31: TSS 0x32 (frame, timestamp), AIS 0x42 (one word per ROC)
   └─ ROC 0x12, 0x13         bank of banks
      ├─ ROC stream info     bank 0xFF30: TSS 0x31, AIS 0x41 (port info)
      └─ Payload banks       tags 1, 3, …, 15; ~74% empty, as in the real data; hit words otherwise
```

A synthetic frame was ~423 bytes on average (the cosmics data: 426), so 256 frames made a ~95 KB event and 4096 frames ~1.5 MB. Those are the event sizes in the results below.

## How to run

Prerequisites as in [lb-test](lb-test.md), steps 1–2: `~/.bashrc` with `E2SAR_BIN` and, for LB runs, `EJFAT_URI`, and e2sar-daos built in `~/daq/e2sar-daos`.
The nodes can't SSH to each other, so start the two roles in two terminals, one per node, with the **same TAG and mode**. They meet through files in the shared `$HOME/e2sar_run/bench/<TAG>/`.

```bash
# ebpf2203 (receiver), start first
bash ~/daq/e2sar-daos/scripts/e2sar_e2e_bench.sh recv run1 -m lb
# ebpf2201 (sender)
bash ~/daq/e2sar-daos/scripts/e2sar_e2e_bench.sh send run1 -m lb -F /path/to/frames.evio
```

`-m lb` goes through the LB in `$EJFAT_URI` (both sides `--withcp`). `-m n2n` sends straight to ebpf2203.
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
| `-m lb\|n2n` | both | `lb` | Through the LB, or node-to-node |
| `-F` | send | `$EVIO_FILE` | EVIO v6 file whose frames are sent. Required |
| `-P` | send | 16 | Segmenters, each with its own producing thread and dataId |
| `-s` | send | 1 | Send sockets (threads) per Segmenter |
| `-n` | send | 6250 | Events per Segmenter (100,000 in total by default). More than the file's frames starts over |
| `-r` | send | 20 | Total rate in Gbps (−1: unlimited) |
| `-T` | recv | 32 | Receive threads (ports). Node-to-node, use T ≥ P |
| `-D`, `-I` | recv | 16, 128 | Dequeue threads, and in-flight puts per thread |
| `-C` | recv | 1024 | DAOS chunk size in KiB (0: whole event in one value). KV sink only |
| `-S kv\|dfuse\|pil4dfs` | recv | `kv` | Removed with dfuse support. The pil4dfs and dfuse results below were taken with it |

Environment overrides: `EVIO_FILE` (instead of `-F`), `SEND_IP`, `RECV_IP`, `PORT` (20600), `POOL` (`sc26`), `CONT` (`e2sar-bench`), `NIC`, `RUN`, and `SEND_ARGS` for extra `e2sar_evio_send` options (`SEND_ARGS="-b 8388608"` for an 8 MiB send socket buffer).

- **Check that the nodes are idle first,** as in [lb-test step 4](lb-test.md). The receiver refuses to start if `PORT` is in use.
- **Clean-up is built in.** The receiver destroys the container after reading it back. The sender only reads its EVIO file. Each run leaves only its logs and CSVs in `$RUN/bench/<TAG>/`. For 100,000 events that's about 10 MB.
- **A run fails** when any event is missing or has the wrong digest. `rx_discards_phy` > 0 almost always comes with it.

## Recommended settings

These are the defaults of `e2sar_evio_send` and `e2sar_e2e_bench.sh`. `e2sar_daos_recv` on its own defaults to `--deq 8 --inflight 128`, from the [receiver tuning](../results/2026-10-29-receiver-tuning.md) of 2026-10-29:

| | Setting | Why |
|---|---|---|
| Sender | 16 Segmenters × 1 socket | Spreads events over many receive threads (node-to-node: ports PORT…PORT+15). One socket each keeps bursts short |
| Sender | 256 frames per event (~95 KB; synthetic, the old `-f`. With a file: `SEND_ARGS="-f 256"`) | Short line-rate bursts, but few enough events to stay far below the receiver's ~85k events/s |
| Sender | `--rate 20` | The only rate with no loss in every run through the LB (3 of 3). Through the LB, up to 35 was lossless in some runs. Node-to-node, no rate tested was lossless every time |
| Receiver | `--threads 32` | Through the LB: more ports, fewer events sharing one. Node-to-node: at least P |
| Receiver | `--deq 16 --inflight 128` | 4 dequeue threads cap at ~24 Gbps. 16 are enough to 40 Gbps |
| Receiver | `-b 1073741824 --timeout 10000` | As before. No socket drops |
| Receiver | `--chunk 1024` | As fast as one value per event, a little faster for events > 1 MiB. See [DAOS layout](../results/2026-10-19-e2e-bench.md#6-daos-layout-chunks-or-one-value-per-event) |
