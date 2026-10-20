# Sending EVIO files instead of synthetic frames: proposed changes

Status: proposal, nothing implemented yet (2026-10-19).

The end-to-end bench ([E2E_BENCHMARK.md](E2E_BENCHMARK.md)) sends synthetic EVIO v6 records. Every event has the same number of frames (`-f`, ~95 KB at 256).
To measure real data, `e2sar_evio_send` would read the records of an EVIO file (`frames_thread0_*.evio`, `EC5_EC6_cosmics_*.evio`, …) and send each one as an E2SAR event. Then the frame count, and the size, differs from record to record.
This page lists what still works, what breaks, and the changes to make in the sender, the analyzer and the bench script.

## What still works

| | Why it carries over |
|---|---|
| Pacing | `produce()` paces by bytes already sent (`due = sentBytes * 8 / gbps`), not by events. With variable sizes, the average rate stays correct |
| Throughput metrics | Received, E2E and Sender are bytes over time. They don't depend on event size |
| Latency join | `send.csv` and the receiver's `timing.csv` both have `bytes` per event. A per-size breakdown needs no format change |
| Receiver and DAOS | `e2sar_daos_recv` stores any event. Records over 1 MiB are split into `--chunk` values as today |

## What breaks or changes

1. **One latency p50 no longer means much.** Every number in [latency per stage](E2E_BENCHMARK.md#latency-per-stage-ms-same-runs) is for one event size. With mixed sizes, a percentile over the run mostly reflects the file's size mix. Two files, or a file and the synthetic run, can't be compared that way.
2. **Size dependence per stage:**

   | Stage | Depends on size | With variable records |
   |---|---|---|
   | sender | A little. 95 KB is ~8 µs on the wire at 100 Gb/s. Most of the measured 0.07–0.1 ms is fixed overhead | Head-of-line blocking: a small record queued behind a large one picks up the large one's send time |
   | network | Barely. Measured from the *last* packet sent | The wait for a dequeue thread depends on the size of the events ahead of it |
   | daos | Yes. One put up to 1 MiB: 0.22 ms at 95 KB, 1.15 ms for 1.5 MB as one value. Above 1 MiB the chunks are written in parallel (0.55 ms at 1.5 MB) | Most of the spread in "total" will come from here |

3. **The largest record sets the loss, not the mean.** Each record leaves the sender as one line-rate burst, and the NIC port discards on the receiver grow with frames per event ([diagnosing the port discards](E2E_BENCHMARK.md#diagnosing-the-port-discards), run 4). A file with occasional very large records can lose events at a rate the synthetic ~95 KB events passed.
4. **Digests.** The check relies on event n being copy `n % pool` of a pool built up front, with digests written per pool slot. File records need their own digests.
5. **File I/O in the send loop.** `queue_ns` is taken after the pacing wait, so read time is not counted as latency. But the schedule is absolute: after a slow read the loop sends back-to-back until it catches up. That burst is exactly what causes port discards.
6. **The event-rate ceiling.** The Reassembler tops out at ~85k events/s whatever the size. Files with many small records hit that before any Gbps limit.
7. **What the latency covers.** It starts when the finished record is handed to the Segmenter. Live, a frame also waits while its record fills: at 65,536 ns per frame, a 256-frame record holds ~16.8 ms of data, far more than the 0.3–0.5 ms node-to-node pipeline latency. A file replay can't measure that wait. Results should say so.

## Proposed changes

### `e2sar_evio_send`: a file mode

| Change | Detail |
|---|---|
| `--file PATH` (repeatable) | Reads EVIO v6 files instead of building synthetic records. `-f`, `--rocs` and `--hits` are ignored with it |
| Parse records, send each as is | Skip the file header (and its index and user header), then take each record by the length in its header, header included, up to the trailer. Check the magic word (byte order) and the version. Reject EVIO v4 for now |
| Compressed records | Send them as they are, since the receiver and the digests only see bytes. Report them as "compressed" in the summary, since their frame count can't be read without decompressing |
| Preload everything before sending | Records go into memory, with digests computed there, before `t0`. No disk I/O or hashing in the timed loop. `--max-bytes` (default e.g. 16 GB) to fail early instead of swapping |
| Records become the pool | Segmenter i gets records i, i+P, i+2P, … as its pool. Event n of Segmenter i is copy `n % pool_i`, as today. With `-n` larger than a share, the file repeats. Digests keep the `<dataId> <slot> <sha256>` format |
| Catch-up cap | `--max-lag-us` (default e.g. 200): if the loop falls further behind schedule than that, move the schedule forward instead of bursting to catch up. Count these resets and print them, so a run that needed them is visible |
| Summary | Records and bytes per file, record size min/p50/p99/max, frames per record min/p50/max (uncompressed only) |
| `--write-file PATH` | Writes the synthetic records to an EVIO file. Sending that file must give the same results as the in-memory synthetic run, which validates file mode |

Splitting the file round-robin over the Segmenters keeps the size mix the same on every dataId. Event numbers and DAOS keys (`<prefix>/<evt>/<dataId>/<chunk>`) don't change.

### `e2sar_e2e_analyze.py`

| Change | Detail |
|---|---|
| Pool size per dataId | Count the slots per dataId in `digests` instead of one global `--pool`. Shares of a file differ by up to one record |
| Latency by size | Next to the overall percentiles, `latency_by_size`: p50/p90/p99/max of each stage per size bin. Bins: < 64 KB, 64–256 KB, 256 KB–1 MiB, > 1 MiB. Leave empty bins out |
| Latency per MB | p50 of daos ms / MB of the event, so files with different mixes can be compared with one number |
| Size stats | Event size min/p50/p99/max in the JSON line and the printed summary |
| Lost events by size | When a run fails, the size distribution of the missing events, to see whether loss follows the large records |

### `e2sar_e2e_bench.sh`

- `-F FILE` on the send side, passed through as `--file`. The `sent` marker and `results.jsonl` record `source=file:<basename>` and the size stats, instead of `frames=`.
- The receive side needs no change.

## How to validate

1. `--write-file` with the defaults (256 frames, 64 slots × 16 Segmenters), then send that file with `-F`: throughput and latency should match the synthetic run within run-to-run noise, at 20 Gbps through the LB and node-to-node.
2. A file of synthetic records with a few large ones mixed in (e.g. `-f 256` with every 50th record `-f 4096`): check that the size bins separate the latencies, and whether port discards appear at rates where pure 95 KB events passed.
3. The real files: `frames_thread0_*.evio` and `EC5_EC6_cosmics_*.evio`, at the [recommended settings](E2E_BENCHMARK.md#recommended-settings), three runs each mode. Report per size bin.
