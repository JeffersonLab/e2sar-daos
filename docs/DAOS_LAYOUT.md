# Data layout in DAOS

## Overview

One run is one container. `T` is a UTC second, `s` a writer slot (0, 1, 2, …: one per running `e2sar_daos_recv`). Example: run `e2sar_1793280000`, two receivers:

```
pool sc26
└── container e2sar_1793280000                     one run
    ├── catalog                                    KV object, string keys
    │     run         start=1793280000 chunk=1048576 oclass=SX index_oclass=S8 batch_threshold=0.9
    │     writer/0    started=1793280000
    │     writer/1    started=1793280001
    │     wm/0        index=1793280042 data=1793280041          (updated once a second)
    │     wm/1        index=1793280042 data=1793280042
    │     done/0      committed=… failed=… lost=…               (when writer 0 stops)
    ├── seconds                                    integer keys: which seconds have index entries
    │     1793280000, 1793280001, …
    ├── index[1793280000]                          integer keys: the batches committed in that second
    │     (0 << 48) | 0    batch table: 5 events, in data[1793280000, 0] at chunk 0
    │     (1 << 48) | 0    batch table: 4 events, in data[1793280000, 1] at chunk 0
    ├── index[1793280001]  …
    ├── data[1793280000, 0]                        byte array: writer 0's event bytes, batches opened in that second
    ├── data[1793280000, 1]                        byte array: writer 1's
    └── data[1793280001, 0]  …
```

- **One run per container.** The container label is the run's name. Deleting a run is destroying its container. [`e2sar_site2ebpf_recv.sh`](../scripts/e2sar_site2ebpf_recv.sh) makes one container per run, `<CONT_LABEL>_<UTC epoch s>`.
- **Mixed object types.** The container is a plain DAOS container (layout type "unknown"). It holds a KV object, integer-key objects and byte arrays. It isn't POSIX, so dfuse can't mount it.
- **Times are UTC seconds** from each receiver's own clock. The receivers need NTP (or PTP). The reader never compares its own clock with the receivers': it decides from the watermarks they publish.
- **No host information.** Writers are known only by their slot.

Reading is described in [DAOS_READER.md](DAOS_READER.md).

## Writer–reader contract

The DAOS container is the only way writers and readers communicate. There are no sockets, signals or shared files between them. The one rule outside the container is the start order: **start the writers first, then the reader.**

| Writers publish (in the container) | Readers use it to |
|---|---|
| `run`: settings (chunk size, classes) | open the arrays and parse the tables |
| `writer/<slot>`: a writer exists | know which watermarks to wait for |
| `index[T]` entries, written after the data | find committed events; anything not in an index entry doesn't exist yet |
| `wm/<slot>` once a second | know when second T is complete |
| `done/<slot>` | know that a writer has finished |

Rules that follow:

- **Writers never read anything a reader writes,** and don't know whether a reader exists. A writer runs at the same speed with zero, one or several readers.
- **Writers first, then the reader.**
  - The first writer creates the container and the `run` record. A reader started before that exits with an error ("container not found: start the writers first"); it doesn't wait.
  - The reader can start any time after that: before the senders, during the run, or after it. A reader started late, or restarted, catches up, since everything it needs is still in the container.
  - A writer that joins late claims the next slot. A running reader picks the new slot up from `writer/<slot>` and waits for its watermark from then on.
- **Readers don't change anything a writer reads.** Readers only read, except the modes that remove data from DAOS once it is in files ([DAOS_READER.md](DAOS_READER.md)). Those never touch the catalog, and destroy the container only once every writer is done.
- **End of run** is decided from the container alone: every claimed slot has `done/<slot>`, the reader has read past every final watermark, and no new slot has appeared for `--end-wait` seconds (default 10).
- **A reader's own state** (restart point, missing-event list) stays in its output directory, never in the container.

## Object ids

DAOS sets the high 32 bits of `oid.hi` (object type and class) with `daos_obj_generate_oid`. `daos container list-objects` and `daos object query` print an object id as two decimal numbers, `hi.lo`. So everything that identifies an object goes in `oid.lo`, as decimal digits that read directly, and the user bits of `oid.hi` are 0:

```
oid.lo = K SSSS TTTTTTTTTT     (decimal digits)
         K: object kind, 1 digit       S: writer slot, 4 digits       T: UTC second, 10 digits
       = K × 10^14 + slot × 10^10 + T
```

| Object | Kind | `oid.lo` | Example `oid.lo` | DAOS object type | Class (default) |
|---|---|---|---|---|---|
| catalog | 1 | `1 0000 0000000000` | `100000000000000` | `DAOS_OT_KV_HASHED` | `S1`, fixed: a reader opens it before it knows the run's classes |
| seconds | 2 | `2 0000 0000000000` | `200000000000000` | `DAOS_OT_MULTI_UINT64` | `--index-oclass` |
| index[T] | 3 | `3 0000 T` | `300001793280042` = index[1793280042] | `DAOS_OT_MULTI_UINT64` | `--index-oclass`, default `S8` |
| data[T, s] | 4 | `4 ssss T` | `400011793280042` = data[1793280042, slot 1] | `DAOS_OT_ARRAY_BYTE` | `--oclass`, default `SX` |

Read an id from the right: the last 10 digits are the second, the 4 before them the slot, the first digit the kind. Slots go up to 9999, and 10-digit seconds last until the year 2286.

- **Placement.** DAOS picks an object's targets from a hash of the whole id, so consecutive seconds land on unrelated targets. Within an object, the hash of the top-level key (dkey) picks the group. The id bits only have to be unique; their decimal layout doesn't affect placement.
- **Classes.** The data class sets throughput: `SX` stripes over all targets. If the container needs redundancy, use `RP_2GX` or an `EC_…GX` class, with a chunk size that is a multiple of a full EC stripe (data shards × EC cell size). The index objects are small and each is read once, so `S8` is enough; use a wider class at very high batch rates.
- **Same classes for the whole run.** The classes are stored in the catalog's `run` record. A receiver that joins a run with different classes, or a different chunk size, stops with an error.

## Catalog

A KV object with string keys. Values are records of `key=value` pairs separated by spaces.

| Key | Value | Written |
|---|---|---|
| `run` | `start=<UTC s> chunk=<bytes> oclass=<data class> index_oclass=<class> batch_threshold=<θ>` | By the first receiver, with a conditional insert (`putIfAbsent`). Later receivers read it and check that their settings match |
| `writer/<slot>` | `started=<UTC s>` | When a receiver starts: it claims the first free slot 0, 1, 2, … with `putIfAbsent` |
| `wm/<slot>` | `index=<S> data=<S'>` | Once a second by each receiver: its watermarks, below |
| `done/<slot>` | final statistics (`committed= failed= lost= batches= …`) | When the receiver stops. Marks the slot as finished |

A receiver prints its slot in its own log ("writer slot 1"), so a slot can be matched to a machine when debugging.

### Watermarks

- `index=S`: every index write this receiver started before second S has completed. No new entry can appear in `index[T]` for T < S, because its clock is already past T.
- `data=S'`: every batch this receiver opened before second S' (so every batch in `data[T, s]` with T < S') is committed: its data write and its index write have both completed. Used by readers that remove data from DAOS, to know when `data[T, s]` can go.

**A second T is complete** when every claimed slot has `index > T` or a `done/<slot>` entry. The reader reads `index[T]` only after that, so it never misses an entry that is still being written. If a slot's watermark hasn't changed for `--stall` seconds (default 30, measured on the reader's clock as time since it last saw the value change; receiver killed), the reader warns and continues without that slot. If the slot's watermark moves again later, its entries for seconds the reader already passed are read and counted as late.

## Batches

An event's bytes go into its receiver's data array, and its description into an index entry. Both are written per **batch**: a group of events written together.

```
small event: size ≤ θ × chunk     (θ = --batch-threshold, default 0.9: ≤ ~943 KB with 1 MiB chunks)
             copied into the dequeue thread's open batch buffer (1 chunk)
large event: written on its own, without a copy, as a batch of one
```

A dequeue thread flushes its open batch when:
- the next small event doesn't fit in the chunk;
- `--flush-us` has passed since the batch's first event (default 5,000 µs), so a quiet stream can't hold events back;
- the UTC second changes, so a batch's data stays in one data second;
- the receiver stops.

## Data arrays: `data[T, s]`

A DAOS byte array (`DAOS_OT_ARRAY_BYTE`, cell size 1 byte), the object type DFS uses for files. `T` is the second in which the batch was opened, `s` the writer slot. Only writer `s` writes to it.

```
byte offset o  →  chunk c = o / chunk  →  dkey c  →  hashed to a target group (SX: any target)
```

- **Chunk size.** `--chunk` KiB (default 1024 = 1 MiB) is the array chunk size and the batch size. It's stored in the catalog, and the reader opens the array with it (`daos_array_open_with_attr`), since a byte array stores no metadata.
- **Every batch starts on a chunk boundary.** A batch of small events is at most one chunk, so its write goes to one target in one request. A large event covers `ceil(size / chunk)` chunks, which DAOS writes to different targets in parallel.
- **Offsets.** Each receiver keeps one atomic chunk counter, shared by its dequeue threads, that never resets. A batch takes `n = ceil(bytes / chunk)` chunks with one `fetch_add(n)` and is written at `offset = first chunk × chunk`. The arrays are sparse: the unused end of a chunk and the chunks used in other seconds are holes, which take no space and read as zeros.
- **Very large events** are written as several array writes of at most `--max-write` (default 64 MiB), so one event can't launch thousands of chunk writes at once. Its index entry is written after the last one completes.

Example: one receiver's array for one second:

```
data[T, slot 1]
 chunk 4810 | batch: ev 812/4321  ev 812/4322  ev 813/4321 … ev 822/4330 | hole |
 chunk 4811 | batch: ev 823/4321 …                                  | hole |
 chunk 4812 | ev 900/4321, a 1.7 GB file: chunks 4812 … 6506, spread over all targets …
 chunk 6507 | batch: ev 901/4321 …
```

## Index: `index[T]` and `seconds`

`index[T]` lists the batches whose data was fully written and whose index write was started in second T (the **commit second**). An event becomes visible to readers when its batch's index entry exists: that entry is the commit marker. The data array uses the second the batch was opened, so a slow write (a 1.7 GB file taking several seconds) shows up in a later index second without holding back the events committed meanwhile.

```
index[T]   dkey = (slot << 48) | batch_seq      batch_seq: per slot, 0, 1, 2, … for the run
                                                e.g. slot 1, batch 391 = 0x0001_0000_0000_0187
           akey = 0
           value = batch table (binary, little endian)

batch table:
  header  char[4] "E2SB"          check value: the ASCII bytes 45 32 53 42 ("E2SAR batch")
          u32 n                  number of events
          u16 slot   u16 reserved   u32 reserved
          u64 T_data             second of the data array, data[T_data, slot]
          u64 offset             byte offset of the batch in that array
          u64 length             bytes from offset to the end of the last event
  n ×     u64 evtNum   u64 offset (from the batch start)   u64 size   u16 dataId   u16 flags   u32 reserved
```

The header is 40 bytes and each entry 32 bytes. A reader checks a table before using it: it starts with `E2SB`, its size is exactly 40 + 32 × n bytes, and its slot matches the dkey. In a hexdump of an index value, `E2SB` shows up as text at the start. A batch of 50 × 20 KB events has a 1.6 KB table.

`seconds` marks the seconds that have index entries, so a reader skips empty seconds without opening them:

```
seconds    dkey = T, akey = 0, value = 1 byte
```

A receiver writes `seconds[T]` the first time it puts an index entry into `index[T]`. Two receivers writing it for the same second write the same value.

## Writer (`e2sar_daos_recv`)

1. **Start.** Open or create the container. Create `run` with `putIfAbsent`, or read it and check the settings. Claim a writer slot. Print the slot.
2. **Each event,** on a dequeue thread (`--deq`), from the Reassembler:
   - small: copy into the open batch, flushing it first if it's full;
   - large: start a batch of one that points at the Reassembler's buffer.
3. **Flush a batch,** asynchronously on the thread's DAOS event queue (`--inflight` operations at once):
   1. take chunks from the counter and `daos_array_write` the batch into `data[T_open, slot]`;
   2. when it completes, `daos_obj_update` the batch table into `index[now]` (and `seconds` if it's this receiver's first entry in that second);
   3. when that completes, count the events as committed and return the buffer to the thread's pool (`--batch-buffers` of one chunk each), or free the Reassembler's buffer.
   The index write is started by the thread after it polls its event queue, not from inside the completion callback.
4. **Back-pressure.** If every in-flight slot or every batch buffer is busy, the thread polls its event queue until one frees up. The Reassembler's queue absorbs short delays ("Lost in enqueue" counts what it can't).
5. **Failures.** If a data write fails, no index entry is written: the batch's events count as failed and are never visible. A failed write is retried once before that.
6. **Once a second,** the stats thread writes `wm/<slot>` from the dequeue threads' oldest pending index write (`index`) and oldest batch not yet committed (`data`).
7. **Stop.** Flush and drain every thread, write the final watermark and `done/<slot>`.

The same event number is never written by two receivers: the LB picks the receiver from the event number alone, so all `dataId`s and segments of event N go to one receiver. A sender that reuses event numbers (for example after a restart) produces duplicates, which the readers report.

**Log lines** start with the UTC time in seconds since the epoch, with milliseconds after the point, not a date and time:

```
1793280042.123 1.02 Gbps, 6012 evt/s | committed 6011 evts, 120.3 MiB, 98 batches | reassembly loss 0, enqueue loss 0
```

Lines from several receivers can then be merged and sorted as numbers, and matched to the seconds in DAOS: the part before the point is `T`.

### Receiver options

| Option | Default | Meaning |
|---|---|---|
| `--pool`, `--cont-name` | required | pool, and the run's container (created if missing) |
| `--chunk` | 1024 KiB | array chunk size = batch size |
| `--oclass` | `SX` | data array class |
| `--index-oclass` | `S8` | seconds and index class |
| `--batch-threshold` | 0.9 | events ≤ θ × chunk are batched |
| `--flush-us` | 5000 | flush a partly filled batch after this long |
| `--batch-buffers` | 8 | batch buffers per dequeue thread |
| `--max-write` | 64 MiB | largest single array write; larger events are split |
| `--deq`, `--inflight` | 8, 128 (to be tuned) | dequeue threads; DAOS operations in flight per thread |

## Examples

### A 1.7 GB EVIO file sent with `e2sar_ft`

`frames_thread0_file0020.evio` (1,776,313,996 bytes) is event 0, `dataId` 4321. It's larger than 0.9 MiB, so it's a batch of one:

```
data[T, 0]   chunks c … c+1694, written as 27 array writes of ≤ 64 MiB, spread over all targets
index[T']    dkey (0 << 48) | 17: header {T_data = T, offset = c × 1 MiB, length = 1776313996, n = 1}
                                   entry  {evtNum 0, offset 0, size 1776313996, dataId 4321}
```

`T'` is the second the last write completed, a few seconds after T. `e2sar_daos_read --offline` restores it byte for byte ([DAOS_READER.md](DAOS_READER.md#--offline-one-file-per-event)). If any packet of the file was lost, E2SAR dropped the whole event and nothing is written to DAOS.

### coda-fb frames, ~20 KB at 2.5 Gbps

One sender, one `dataId`, ~15,600 frames per second, split by the LB over two receivers (~7,800 per second each). With 8 dequeue threads per receiver, each thread gets ~1,000 frames per second, so a 5 ms flush catches ~5 frames (~100 KB) per batch:

```
data[T, 1]   chunk 2000: frames 1043, 1047, 1048, 1052, 1053 (~100 KB) | hole |
index[T]     dkey (1 << 48) | 391: header {T_data T, offset 2000 × 1 MiB, length ~100 KB, n 5}, 5 entries
```

With the default `--interval 5`, `e2sar_daos_read --live` writes one ~1.6 GB file per 5 seconds with ~78,000 frames in frame order. Fewer dequeue threads or a longer flush fill the batches more: see the tuning plan in [STREAMING_WORKFLOW.md](STREAMING_WORKFLOW.md#tuning).

## Cost per event

| Event size | DAOS operations |
|---|---|
| 1.7 GB | 27 array writes (1,695 chunk writes inside DAOS) + 1 index write |
| 0.9 MiB – 2 MiB | 1 array write + 1 index write |
| 95 KB | ~0.2 (10 events per batch) |
| 20 KB | ~0.04 with full batches |
| Finding events to read | one index listing per second |

The receive side (E2SAR and the NIC, ~25 Gbps lossless in [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md)) remains the limit. The few DAOS operations show up as few dequeue threads and little CPU for the rate, and a reader whose cost doesn't grow with the run.
