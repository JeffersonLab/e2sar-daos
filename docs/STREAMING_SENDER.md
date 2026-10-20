# Streaming real time frames: proposed changes to the sender

Status: proposal, nothing implemented yet (2026-10-19).

The end-to-end bench ([E2E_BENCHMARK.md](E2E_BENCHMARK.md)) sends synthetic EVIO v6 records from a pool built up front. Every record holds the same number of frames (`-f`, ~95 KB at 256). The sender paces them to `--rate`.
A streaming DAQ works differently. Time frames arrive one after another, one every 65,536 ns per stream, each with its own size. Records are closed when a policy says so, so the number of frames, and the size, differs for every record.
This page describes a streaming mode for `e2sar_evio_send` that works like that, using real frames, and what it changes in the sender, the analyzer, the bench script and the results.

## What a stream looks like

| | Now (synthetic pool) | Streaming mode |
|---|---|---|
| Frames | Synthetic, ~423 B on average | Real 0xFF60 built time frames, each with its own size |
| Rate | `--rate` Gbps, paced by bytes | Set by the frame clock: streams × frame size / frame period |
| Frames per record | Fixed (`-f`) | Variable: closed by size, by time, or at the boundaries given by the source |
| Event content | Copy `n % pool` of 64 records | Every record distinct |
| Event number | 1…N | The number of the record's first frame |
| Latency starts | When the finished record is handed to the Segmenter | When the record's first frame is released. The wait while the record fills is a new stage |

One stream of real frames (~426 B every 65.536 µs) is only ~52 Mb/s. 16 streams in real time are ~0.83 Gbps. To reach the bench's 20 Gbps, the frame clock has to run about 24× faster than real time (`--speedup`).

## Proposed changes

### `e2sar_evio_send`: a streaming mode

| Change | Detail |
|---|---|
| Frame source: `--frames-from FILE` (repeatable) | Extracts the 0xFF60 built time frame banks from recorded CODA streaming files (`frames_thread0_*.evio`, `EC5_EC6_cosmics_*.evio`) into memory before the start. Their contents and sizes stay as they are. When the run needs more frames than the files hold, they repeat, with the frame number and timestamp in the stream info bank (TSS 0x32) rewritten so they keep counting up. Without the option, the synthetic frames serve as the source |
| Source interface | Frames come through one interface (`next frame, its release time`). Later, a live source (frames from an aggregator) can be added without changing the record building or the timing |
| One stream per Segmenter | Segmenter i is stream i, with its own frame clock, dataId and frame numbering. Each stream starts at its own offset in the frames, so the streams don't send identical data |
| Frame clock: `--speedup S` | Frame k of a stream is released at `t0 + k × 65,536 ns / S`. Replaces `--rate`. The rate a run reached is reported, not set |
| Record policy | `--record-frames F` closes a record after F frames (the current behavior, for comparison). `--record-bytes B` closes it before the frame that would push it past B bytes, so the number of frames varies. `--record-as-source` keeps the record boundaries of the source files. `--flush-us T` also closes a record once its first frame is T µs old, so a quiet stream can't hold frames back |
| Record building | Writes the record header and index for whatever frames the record got, as `buildRecord()` does now. Header fields: record number, event count = frames, index length |
| Event number | The first frame's number. It counts up per stream, so DAOS keys `<prefix>/<evt>/<dataId>/<chunk>` stay unique and sort in time order |
| Buffers | A record's buffer has to live until the Segmenter's send callback. It is taken from a free list per stream and returned in `onSent`, so there's no allocation in the steady state |
| Digests | SHA-256 per record, computed after `addToSendQueue` (the buffer doesn't change until it is sent), so it doesn't add to the build time. Written as `<dataId> <evt> <sha256>` |
| Back-pressure | A live source can't wait. If the Segmenter's queue is full, the stream falls behind its frame clock. Record the largest lag, count frames released more than `--max-lag-us` (default e.g. 1000) late, and mark the run as failed if there are any, because a live source would have overflowed |
| Timing CSV | Adds `frames`, `first_frame_ns` and `last_frame_ns` (wall-clock release times of the first and last frame) to `evt_num,data_id,bytes,queue_ns,sent_ns` |
| Summary | Streams, speedup, frames and records sent, frames per record min/p50/max, record size min/p50/p99/max, largest lag, late frames |

### `e2sar_e2e_analyze.py`

| Change | Detail |
|---|---|
| Digests by event | Look up `(dataId, evt)` directly, without a pool. Keep the pool lookup for the synthetic mode |
| New stage: build | `first_frame_ns → queue_ns`: the oldest frame's wait while its record fills. Its real-time equivalent is the build time × speedup. Report both |
| Data age | `first_frame_ns → commit`, next to today's `total` (`queue_ns → commit`) |
| Latency by size | p50/p90/p99/max of each stage per record size bin: < 64 KB, 64–256 KB, 256 KB–1 MiB, > 1 MiB. Leave empty bins out. Also daos ms per MB, to compare runs with different size mixes with one number |
| Size and frame stats | Record size and frames per record min/p50/p99/max in the JSON line and the printed summary |
| Lost events | When a run fails, the size distribution of the missing records, to see whether loss follows the large ones |

### `e2sar_e2e_bench.sh`

- Send side: `-F FILE` (frame source), `-x SPEEDUP`, and one of `-f FRAMES`, `-b BYTES` or `-a` (as in the source) for the record policy, plus `-t FLUSH_US`.
- `sent` and `results.jsonl` record the source, the speedup and the policy instead of `frames=` and `rate=`.
- The receive side needs no change.

## What changes in the results

1. **Latency has a new, larger stage.** The first frame of a 256-frame record waits ~16.8 ms in real time for the record to fill (256 × 65,536 ns). That's far more than the node-to-node pipeline latency of 0.3–0.5 ms p50. The record policy, not E2SAR or DAOS, then sets most of the data age, except through the LB, where the ~75 ms WAN round trip comes first.
2. **One p50 over the run doesn't mean much.** With variable records, a percentile mostly reflects the size mix. Report the stages per size bin.
3. **Each stage depends on the record size differently:**

   | Stage | Depends on size | With variable records |
   |---|---|---|
   | build | Yes. Real time: frames × 65.5 µs | Follows the record policy directly |
   | sender | A little. 95 KB is ~8 µs on the wire at 100 Gb/s. Most of the measured 0.07–0.1 ms is fixed overhead | A small record queued behind a large one picks up the large one's send time |
   | network | Barely. Measured from the *last* packet sent | The wait for a dequeue thread depends on the size of the events ahead of it |
   | daos | Yes. One put up to 1 MiB: 0.22 ms at 95 KB, 1.15 ms for 1.5 MB as one value. Above 1 MiB the chunks are written in parallel (0.55 ms at 1.5 MB) | Most of the spread after the build stage |

4. **The largest record sets the loss, not the mean.** Each record leaves the sender as one line-rate burst. The receiver's NIC port discards grow with frames per event ([diagnosing the port discards](E2E_BENCHMARK.md#diagnosing-the-port-discards), run 4). `--record-bytes` limits the burst directly. With `--record-as-source`, occasional large records can lose events at rates the 95 KB synthetic records passed.
5. **Small records hit the event-rate ceiling.** The Reassembler tops out at ~85k events/s whatever the size. At 16 streams × speedup 24 (~20 Gbps), the streams release ~5.9M frames/s. Records must then average at least ~70 frames, and more for margin. At 256 frames that's ~23k records/s, the same as the bench now.
6. **Pacing becomes smoother within a stream, but streams can line up.** Records are released by the frame clock, not by a byte schedule, so there are no catch-up bursts. But streams with the same policy and start time close their records at the same moment. Start the streams with random phases, or measure both, to see whether aligned bursts change the port discards.
7. **Through the LB, event numbers are the LB's ticks.** The LB sends the events of all data sources with the same tick to the same receiver. Records that start at different frames on each stream have unrelated ticks, so the same time slice of different streams goes to different receivers. That doesn't matter for writing to DAOS, which keeps every record. It does matter if a later step builds events across streams. Then the streams need common record boundaries (e.g. on multiples of F frames), which a byte-limited policy doesn't give.

## How to validate

1. **Same as the bench now:** synthetic frames, `--record-frames 256`, speedup chosen for 20 Gbps, 16 streams. Throughput and pipeline latency should match [the confirmation runs](E2E_BENCHMARK.md#5-confirmation-runs-with-the-defaults) within run-to-run noise, through the LB and node-to-node. The build stage should come out at 256 × 65.536 µs / speedup.
2. **Variable records:** the same, with `--record-bytes 98304`. Check the per-size bins and that frames per record now vary.
3. **Real frames:** `--frames-from` the cosmics and `frames_thread0` files, with `--record-bytes` and `--record-as-source`, at the [recommended receiver settings](E2E_BENCHMARK.md#recommended-settings), three runs per mode. Report per size bin, the build stage in real-time units, the largest lag and the late frames.
4. **Stream alignment:** run 3 with and without random phases, node-to-node at 25–30 Gbps, and compare `rx_discards_phy`.
