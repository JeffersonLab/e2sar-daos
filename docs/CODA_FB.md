# coda-fb as the future sender: comparison with e2sar_evio_send and rate estimates

Status: notes, nothing implemented (2026-10-20). Based on [JeffersonLab/coda-fb](https://github.com/JeffersonLab/coda-fb) at `cc8b3d5` (2026-09-25) and on the recorded coda-fb output `frames_thread0_file0020.evio`.

[coda-fb](https://github.com/JeffersonLab/coda-fb) is the CODA frame builder. It will produce the built time frames that e2sar-daos stores. Today `e2sar_evio_send` ([E2E_BENCHMARK.md](E2E_BENCHMARK.md)) stands in for it with synthetic frames.
This page covers what coda-fb does now, how its output compares with what `e2sar_evio_send` sends, and the rates to expect when it sends to `e2sar_daos_recv`.

## What coda-fb does now

coda-fb **sends nothing over the network**. It is a receiver: it sits after the EJFAT LB, on the same side as `e2sar_daos_recv`.

```
ROCs ──UDP──▶ EJFAT LB ──▶ coda-fb
                             E2SAR Reassembler, N threads on ports P..P+N-1   (--threads)
                             reception loop: check the CODA block header, read ROC id, frame number, timestamp
                             FrameBuilder, M threads (--fb-threads), one FIFO per stream (ROC)
                             ──▶ EVIO-6 records ──▶ ET system and/or .evio files (2 GB per file)
```

**Input:** one E2SAR event per ROC per time frame. The payload is an 8-word CODA block header (word 8 = `0xc0da0100`), then the ROC bank: ROC id in word 10, 0xFF30 stream info in word 12, the 0x31 time slice segment with the frame number in word 14 and the 64-bit timestamp in words 15–16.

**Aggregation:** slices go to builder thread `frameNumber % M`. Each builder keeps one FIFO per stream. It fixes a constant event number offset per stream at the start and builds a frame once every stream has the same corrected event number, or after `--frame-timeout` (1 s) with the streams it has. It aligns on the **Reassembler's event number** (the LB tick), not on the payload timestamp.

**Output:** one EVIO-6 record per built frame:

```
Record header (14 words, bit info 0x80004206, record number = frames built by this thread, 1 event, no index)
└─ bank 0xFF60, bank of banks, num = error bit (7) | slice count (0–6)
   ├─ bank 0xFF31, stream info: TSS 0x32 (frame number, timestamp = mean over ROCs), AIS 0x42 (ROC id << 16 | status, one word per ROC)
   └─ the ROC banks, copied as received (CODA block header removed), padded to 4 bytes
```

That record is written to an ET event (up to `--et-event-size`, 2 MiB) and/or appended to `frames_thread<N>_file<MMMM>.evio`. To send it to e2sar-daos, coda-fb needs a new output: see [How coda-fb could send](#how-coda-fb-could-send).

## Similarities and differences

### What is the same

| | Both |
|---|---|
| Data layout | EVIO v6, big endian, bit info `0x80004206`. A built time frame is bank 0xFF60 containing 0xFF31 (TSS 0x32 + AIS 0x42) and one bank per ROC. `e2sar_evio_send` was modeled on coda-fb files, and the bench verifies against them |
| Frame clock | One frame every 65,536 ns per stream. The timestamp advances 65,536 per frame (measured in coda-fb output: median step 65,536) |
| Frame size | ~350–425 B. Synthetic: ~423 B. coda-fb output: 352 B mean per frame (min 292, p99 400, max 624), 2 ROCs |
| E2SAR | Both build on E2SAR 0.4.0rc1 (coda-fb requires ≥ 0.4.0rc1). coda-fb's input side is the same `Reassembler` with `--withcp` that `e2sar_daos_recv` uses: N ports from P, registers with the LB, deregisters on Ctrl-C |

### What is different

| | coda-fb | `e2sar_evio_send` |
|---|---|---|
| Role | Receiver and aggregator. LB → ET/file | Sender. Memory → Segmenter → LB or node-to-node |
| Network output | **None** today (ET or files) | E2SAR `Segmenter`, P of them, MTU 9000 |
| Data | Real ROC data, copied as received | Synthetic, built at start. Event n is copy n mod 64 |
| Frames per record | **1** (1 event per record, no index array) | F (`-f`, default 256 ≈ 95 KB), with an index array |
| Record overhead | 56 B header per frame: 407 B record for a 352 B frame (+16%) | 56 B + 4 B index entry per F frames (<1% at F = 256) |
| Rate | Set by the experiment: streams × 15.26 kHz × frame size. Cannot be dialed up | `--rate` Gbps, paced by bytes |
| Frame order | Not in frame number order. In `frames_thread0_file0020.evio`: 77% of steps +1, 17% −1, rest +2/+3. M builder threads, plus partial frames after a timeout | In order: frame n in event ⌊n / F⌋ |
| 0xFF60 `num` | Error bit and slice count | Number of ROCs |
| Timestamp | Mean of the ROCs' timestamps | n × 65,536 |
| Missing data | A frame is built after 1 s with the ROCs it has; the error bit is set | Never happens |
| IDs | Input dataId = ROC id. Output: no E2SAR ids (ET/file) | dataId 4321 + i, eventSrcId 1234 + i per Segmenter |
| Ordering key | LB tick (Reassembler event number) | Event number 1…N per Segmenter |
| Verification | `evio_event_parser` checks structure | SHA-256 of every event, read back from DAOS |

The difference that matters most for e2sar-daos is **frames per record**. coda-fb emits one ~400 B record per frame. At the E2SAR event rate limit, that is far too small to be efficient (see below).

## Rate estimates

### Symbols

| Symbol | Meaning | Value |
|---|---|---|
| T_F | Frame period | 65,536 ns |
| f_F = 1 / T_F | Frame rate per stream | 15,259 frames/s |
| N_roc | ROCs (streams) per frame | 2 in the recorded data |
| S_F | Built frame size (bank 0xFF60) | 352 B mean (measured) |
| S_hdr | EVIO record header | 56 B |
| F | Frames per E2SAR event | 1 for coda-fb today; 256 in the bench |
| S_E = S_hdr + 4·F·[F>1] + F·S_F | Event size | 407 B at F = 1; ~90 KB at F = 256 |
| S_seg | E2SAR payload per packet at MTU 9000 | 9000 − 20 (IP) − 8 (UDP) − 16 (LB) − 20 (RE) = 8,936 B |
| R_max | Reassembler event rate limit | ~85k events/s, any size ([E2E_BENCHMARK.md](E2E_BENCHMARK.md), section 1) |
| B_max | Highest lossless rate measured | ~25–35 Gbps through the LB (varies by run and day), ~25–30 node-to-node |

### Real-time rate of one coda-fb

All frames, at their real rate:

```
frames/s  = f_F                     = 15,259 /s
records/s = f_F / F                 = 15,259 /s at F = 1
bandwidth = f_F · S_E(F) · 8 / F    = 15,259 × 407 × 8 = 49.7 Mb/s at F = 1
                                      15,259 × 352 × 8 = 43.0 Mb/s of frames, as F grows large
packets/s = (f_F / F) · ⌈S_E / S_seg⌉ = 15,259 /s at F = 1 (each record fits in one packet)
```

One coda-fb in real time is **~50 Mb/s and ~15k events/s**. For comparison, LB 638 already carries ~15k packets/s of ~100 B from 129.57.69.58. That is consistent with one ROC stream sending one nearly empty slice per frame into a coda-fb.

Input to coda-fb, for comparison: N_roc slices per frame, each about (S_F − 44) / N_roc + 32 B, where 44 B is the 0xFF60 and 0xFF31 headers and 32 B is the CODA block header that coda-fb strips:

```
input events/s = N_roc · f_F                   = 2 × 15,259 = 30,518 /s
input bytes/s  ≈ f_F · (S_F − 44 + 32·N_roc)   = 15,259 × 372 × 8 ≈ 45 Mb/s
```

### What e2sar-daos can take

The receiver is limited by events/s and by bits/s, whichever is hit first:

```
B(F) = min( R_max · S_E(F) · 8 ,  B_max )
```

| F | S_E | Limit by events (R_max · S_E · 8) | Limit | Real-time coda-fb outputs one receiver can take: B(F) / (f_F · S_E · 8 / F) |
|---|---|---|---|---|
| 1 | 407 B | 0.28 Gbps | events/s | **5.6** |
| 16 | 5.7 KB | 3.9 Gbps | events/s | 89 |
| 64 | 22.8 KB | 15.5 Gbps | events/s | 355 |
| 256 | 91 KB | 62 Gbps | bandwidth (B_max ≈ 35) | ~800 |

The F = 1 row agrees with what was measured: 1 frame per event reached 0.29 Gbps at 85,396 events/s ([E2E_BENCHMARK.md](E2E_BENCHMARK.md), section 1, row 1).

**Smallest F for a target rate B:**

```
F_min = B / (8 · S_F · R_max)
```

For example, 20 Gbps needs F ≥ 20×10⁹ / (8 × 352 × 85,000) ≈ **84 frames per event**. Leave margin, because R_max is a ceiling, not a working point: F = 128–256 is the safe range.

**Real-time streams needed to fill a target rate B:**

```
N_streams = B / (f_F · S_F · 8)
```

At 20 Gbps that's 20×10⁹ / 43.0×10⁶ ≈ **465 coda-fb outputs** (or builder streams) at the current frame size. One coda-fb alone can't load the pipeline. Higher rates come from many streams, or from larger frames (more ROCs, more hits per frame).

### Latency added by batching

A frame has to wait for its event to fill:

```
t_batch = F · T_F
```

| F | t_batch (real time) |
|---|---|
| 1 | 0.066 ms |
| 84 | 5.5 ms |
| 256 | 16.8 ms |

Through the LB, the ~75 ms WAN round trip still dominates. Node-to-node, batching takes over from DAOS (0.3–0.5 ms p50) as the largest stage once F is more than about 8.

### Loss

Each E2SAR event leaves the sender as a line-rate burst of ⌈S_E / S_seg⌉ packets. Loss on the ebpf2203 port follows burst size ([E2E_BENCHMARK.md](E2E_BENCHMARK.md#diagnosing-the-port-discards)). At F = 256, 11 packets per event passed at 35 Gbps through the LB. Capping the event at about 100 KB keeps coda-fb's bursts within what was tested.

## How coda-fb could send

Two options, both needing work in coda-fb or next to it:

1. **Segmenter output in coda-fb.** Add a sink next to ET and file that collects F built frames per stream into one EVIO record, the way `e2sar_evio_send` builds records, and hands it to a `Segmenter`. That is the "live source" in [STREAMING_SENDER.md](STREAMING_SENDER.md). The record policy (`--record-frames`, `--record-bytes`, `--flush-us`) applies unchanged.
2. **ET forwarder.** A separate process attaches to coda-fb's ET system, batches the frames and sends them with a Segmenter. coda-fb doesn't change, but the frames take an extra copy.

Either way:

- **Event number.** Use the first frame's number, as the streaming proposal does. Because coda-fb emits frames out of order, a record can't be closed after "the next F frame numbers". Close it by count or bytes instead, and keep the first frame number, or sort within a short window first.
- **dataId.** One per builder thread, so DAOS keys `<prefix>/<evt>/<dataId>/<chunk>` don't collide between threads.
- **Back-pressure.** coda-fb receives from ROCs it can't pause. If the Segmenter's queue fills, it should drop and count, not block the builder threads.

## How to check these numbers

1. Run `bin/e2sar_evio_send --frames-from` (once it exists) on `frames_thread0_file0020.evio` at F = 1, 16, 84 and 256 into `e2sar_daos_recv`, and compare the received rate with B(F) above.
2. Read the real input rate from the ESnet border counters or from coda-fb's own statistics (`--report-interval`) during a live run, and compare with N_roc · f_F.
3. Measure S_F on newer coda-fb output with more ROCs. Every number here scales with it.
