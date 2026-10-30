---
type: Reference
title: "e2sar_daos_recv"
description: "The receiver: how writing works, every option, synthetic mode, the progress line and the Timing block."
tags: [e2sar_daos_recv, options, timing]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/bin/README.md", title: "bin/README.md at 0d85d00" }
---

# e2sar_daos_recv

Receives events from an E2SAR sender (e.g. `e2sar_perf -s` or `e2sar_ft -s`) and writes them to DAOS: small events batched into 1 MiB chunks, large ones written directly, all asynchronously. One run per container. Built by `meson compile -C build` into `build/bin/`.

## Prerequisites

- A running `daos_agent` on the receiving node, and access to the pool (default `sc26`).
- One run is one container. `e2sar_daos_recv` creates it on first use; `--cont-name` is its label, required unless `--dryrun`. [`e2sar_site2ebpf_recv.sh`](../../scripts/e2sar_site2ebpf_recv.sh) makes one per run, `<CONT_LABEL>_<UTC epoch s>`, e.g. `e2sar_1793280000`. Pass the same label to `e2sar_daos_read --cont`.
- Without a control plane (no `--withcp`), the sender sends straight to the `data=` address in `EJFAT_URI`. The port there defaults to 19522, so start the receiver with `--port 19522`, or put an explicit port in the URI, e.g. `data=<ip>:10000`.

## Data layout in DAOS

A container holds one run: a catalog (run settings, one writer slot per receiver, their watermarks and final statistics), the event bytes in byte arrays `data[T, slot]`, and per UTC second an index `index[T]` of the batches committed in it. See [daos-layout](../concepts/daos-layout.md) for the layout and [e2sar_daos_read](e2sar_daos_read.md) for reading.

## How writing works

```
Reassembler ──recvEvent──▶ dequeue thread 0 ──▶ batch ──▶ own DAOS event queue (≤ --inflight operations)
            ──recvEvent──▶ dequeue thread 1 ──▶ batch ──▶ own DAOS event queue
                                                    └── shared container, catalog, chunk counter
```

- At start, the receiver creates the run record (or checks that its settings match the run's) and claims a writer slot, which it prints (`Writer slot: 1`).
- Each dequeue thread (`--deq`) owns one DAOS event queue, because event queues must be polled by a single thread.
- Events up to `--batch-threshold` × `--chunk` (0.9 MiB by default) are copied into the thread's open batch, which is written when the next event doesn't fit, after `--flush-us`, when the UTC second changes, or at stop. Larger events are written without a copy, in pieces of at most `--max-write`.
- A batch is committed when its table is written into `index[T]` after its bytes: only then can readers see its events.
- When every operation slot or batch buffer is busy, the thread waits on its event queue. That is the back-pressure: if DAOS can't keep up, the Reassembler queue fills, and the loss shows up as *enqueue loss* in the progress line.
- Once a second the receiver writes its watermarks to the catalog. On Ctrl-C the threads stop receiving, flush and drain everything in flight; then the worker is deregistered and the final statistics are printed and written to the catalog as `done/<slot>`.

## `e2sar_daos_recv`

```bash
e2sar_daos_recv -u '<EJFAT_URI>' --ip <listen ip> --pool sc26 --cont-name <run> [options]
```

E2SAR options (same meaning as `e2sar_ft -r`):

| Option | Default | |
|---|---|---|
| `-u, --uri` | `$EJFAT_URI` | EJFAT URI |
| `-c, --withcp` | off | Use the control plane (register worker with the LB) |
| `--ip` / `--autoip` | — | Listen address (exactly one is required) |
| `--port` | 10000 | Starting UDP port |
| `--threads` / `--cores` | 1 | E2SAR receive (reassembly) threads, or cores to pin them to |
| `-b, --bufsize` | 3 MB | Socket receive buffer |
| `--timeout` | 500 | Event reassembly timeout (ms) |
| `--numa` | -1 | Bind memory to this NUMA node |
| `-4`, `-6`, `-v` | | Prefer IPv4/IPv6 for the control plane; skip cert validation |

DAOS options (the layout options must be the same on every receiver of a run; a receiver with different ones stops with an error):

| Option | Default | |
|---|---|---|
| `--pool` | `sc26` | Pool label (must exist) |
| `--cont-name` | — (required unless `--dryrun`) | The run's container label (created if missing). Every receiver of a run uses the same one |
| `--chunk` | 1024 | Array chunk size = batch size, in KiB |
| `--oclass` | `SX` | Data array object class |
| `--index-oclass` | `S8` | Seconds and index object class |
| `--batch-threshold` | 0.9 | Events up to this fraction of a chunk are batched |
| `--flush-us` | 5000 | Write a partly filled batch after this many µs |
| `--batch-buffers` | 8 | Batch buffers (one chunk each) per dequeue thread |
| `--max-write` | `64M` | Largest single array write; larger events are split |
| `--deq` | 8 | Dequeue threads, each with its own event queue. Independent of `--threads` |
| `--inflight` | 128 | DAOS operations in flight per dequeue thread |
| `--report` | 1 | Progress line interval in seconds (0 = off) |
| `--log-idle` | off | Print a progress line every interval even when nothing changed |
| `--verbose` | off | Print every committed batch |
| `--timing-csv FILE` | — | Write one row of timing per committed event (see [Timing and throughput](#timing-and-throughput)) |

Synthetic mode (no sender, no network):

| Option | Default | |
|---|---|---|
| `--synthetic N` | 0 (off) | Store N events made in memory instead of receiving. The E2SAR options are ignored and `--ip` is not needed. Can't be combined with `--dryrun` |
| `--event-size` | `95K` | Bytes per event, with an optional `K`/`M`/`G` suffix (binary), e.g. `1.5M` |
| `--rate` | 0 | Pace events to this many Gbps. 0 = as fast as DAOS takes them |
| `--dryrun` | off | Receive and reassemble events, then discard them. No DAOS at all. Can't be combined with `--synthetic` |

Exit status: 0 if every received event was committed, 2 if any event failed, 1 on a setup error.

### Synthetic mode: the storage ceiling without a sender

`--synthetic N` replaces the network and the Reassembler with an in-memory generator. Everything after that is the normal receiver: the same threads, batching, layout and `Timing:` block. The run ends by itself after the last event is committed.

```bash
# 100,000 x 20 KiB events, as fast as DAOS takes them
e2sar_daos_recv --synthetic 100000 --event-size 20K --pool sc26 --cont-name synth_1
# the same paced at 2.5 Gbps, to see batching and latency at a given load
e2sar_daos_recv --synthetic 100000 --event-size 20K --rate 2.5 --pool sc26 --cont-name synth_2
```

Unpaced, `DAOS commit throughput` is the most DAOS commits with these settings. An end-to-end run with the same settings can't do better, so the gap between the two is what the network and reassembly cost.

- Each event is a new buffer, filled with a copy of a fixed pseudo-random pattern. Its first 8 bytes are the event number (little-endian), from 0, and the data id is 0. Two synthetic receivers in one run therefore store the same event numbers.
- `Ingest throughput` is the generation rate. With `--rate`, event *i* is released at *i* × size × 8 / rate after start; if DAOS falls behind, events are released late, never dropped.

### Timing and throughput

Every log line starts with the UTC time in seconds, with milliseconds. The progress line, every `--report` seconds, shows the rate over the last interval (illustrative numbers):
```
1793280042.123 in 61000 evts, 2.50 Gbps | committed 60950 evts, 1190.43 MiB, 2.50 Gbps, 15260.00 evt/s, 610.00 batch/s (25.02 evt/batch) | failed 0 | reassembly loss 0, enqueue loss 0
```
`in` is what E2SAR handed over (network side). `committed` is what DAOS has fully stored, and `evt/batch` how full the batches are.

Zero suppression: an interval with nothing new prints no line. The first idle interval prints `idle: no new events; progress lines resume when events arrive`. `--log-idle` prints every interval.

At exit, after the Reassembler and DAOS counters, a `Timing:` block summarizes the run:

| Line | Meaning |
|---|---|
| Receive window | From the first event handed over to the last one. Idle time before the sender starts and after it stops is excluded |
| Commit window | From the first event received to the last event committed to DAOS |
| Ingest throughput | Bytes received / receive window: what the network + reassembly delivered |
| DAOS commit throughput | Bytes committed / commit window: **the end-to-end number to quote** |
| Event latency | From an event being handed over to its batch's index entry being written. Includes the batch's fill time (≤ `--flush-us`) |
| `daos_array_write` latency | Each data write, from launch to completion |
| Index write latency | Each index entry write, from launch to completion |

- A window is measured between events, so with only a few events, throughput is overestimated. Send at least tens of events.
- Percentiles come from a log-scale histogram, accurate to within 12.5%. Min, mean and max are exact.
- If ingest throughput is well below the sender's rate and `reassembly loss` and `enqueue loss` are 0, the dequeue threads are probably behind: try more `--deq`.

`--timing-csv FILE` writes one row per committed event, with times in seconds since the program started (`chunks`: chunks of the event's batch):
```
evt_num,data_id,bytes,chunks,thread,recv_s,commit_s,latency_ms
0,4321,20480,1,0,3.214001,3.219950,5.949000
```
