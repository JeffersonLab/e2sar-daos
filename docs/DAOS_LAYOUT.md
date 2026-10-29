# Data layout in DAOS

How `e2sar_daos_recv` stores reassembled E2SAR events in DAOS, and how `e2sar_daos_read` finds them again.

The layout is defined in code in two headers, which both the writer and the reader use:

- [`include/e2sar_daos/run_catalog.hpp`](../include/e2sar_daos/run_catalog.hpp): objects, object ids and the run catalog
- [`include/e2sar_daos/event_layout.hpp`](../include/e2sar_daos/event_layout.hpp): the keys and metadata of one event

There are three storage modes:

| Mode | Option | Where one event goes |
|---|---|---|
| Per-run KV object (default) | `--cont-name` | Chunk values plus one metadata value in the run's own KV object |
| Shared KV object (old) | `--oid` | The same keys, in one object shared by all runs, without the run catalog |
| dfuse | `--dfuse DIR` | One POSIX file, `DIR/<prefix>/<prefix>_<evtNum>_<dataId>.evio` |

## Overview

Each run (`--prefix`) goes into its own KV object (`DAOS_OT_KV_HASHED`) in `<pool>/<cont>`. A catalog object in the same container lists the runs:

```
pool                    --pool, e.g. sc26
└── container           --cont-name, e.g. e2sar_1793280000 (created on first use)
    ├── run catalog     KV object, OID 0xE25A0000.0x0
    │   ├── run/<prefix>           oid, oclass, chunk size, creation time, receiver that created it
    │   ├── oid/<object id>        <prefix>  (claims the id)
    │   └── recv/<prefix>/<host>   that receiver's final statistics, written when it stops
    └── run object      one per --prefix: OID 0xE25A0001.<FNV-1a 64-bit hash of the prefix>, class --oclass
        └── keys        <prefix>/<evtNum>/<dataId>[/<chunk>], one KV value each
```

- **Container:** `e2sar_daos_recv` creates it on first use (`daos_cont_create_with_label`). `--cont-name` is used as given. [`e2sar_site2ebpf_recv.sh`](../scripts/e2sar_site2ebpf_recv.sh) makes one container per run, `<CONT_LABEL>_<UTC epoch s>`.
- **One object per run, shared by its receivers:** the object id is a hash of the prefix, so receivers on several nodes (e.g. ebpf2201 and ebpf2203 behind the LB) write the same run into the same object without talking to each other. The first one creates the catalog entry; the others find it.
- **Same object class on every receiver of a run:** the class is part of the DAOS object id, so a receiver that joins a run with a different `--oclass` stops with an error.
- **Hash collisions:** two prefixes with the same hash would share an object. The catalog entry `oid/<id>` claims the id for one prefix, and a receiver with another prefix and the same id stops with an error.
- **Why:** listing the runs reads only the small catalog (`e2sar_daos_read --prefixes`), reading one run scans only that run's keys instead of every run's, and deleting a run deletes one object (`e2sar_daos_read --prefix P --delete`) instead of the container.

## Object ids

DAOS leaves the low 32 bits of `oid.hi` and all of `oid.lo` to the user. It sets the high 32 bits of `oid.hi` itself (object type and class).

| Object | `oid.hi` (user bits) | `oid.lo` |
|---|---|---|
| Run object | `0xE25A0001` (`kRunOidHi`) | FNV-1a 64-bit hash of the prefix |
| Run catalog | `0xE25A0000` (`kCatalogOidHi`) | `0` |
| Shared object (old) | `0` | `--oid`, default `0xE25A` (`kLegacyOidLo`) |

FNV-1a is used because it gives the same value on every node and build. Keys inside every object type start with the prefix, so the event layout and the reader's scan are the same for run objects and the shared object.

## Run catalog

Catalog values are records of `key=value` pairs separated by spaces:

```
run/<prefix>          oid=<16 hex> oclass=<name or -> chunk=<bytes> created=<unix s> host=<receiver that created the entry>
oid/<16 hex>          <prefix>
recv/<prefix>/<host>  that receiver's final statistics (committed, failed, lost, ...)
```

- The first receiver of a run creates `run/<prefix>` with a conditional insert (`putIfAbsent`). The others read it and check the object class.
- `oid/<16 hex>` claims the object id for one prefix, so two prefixes with the same hash are caught instead of sharing one object.
- Each receiver writes `recv/<prefix>/<host>` when it stops. `e2sar_daos_read --prefixes` sums these per run.

## One event

Each reassembled event, identified by (`evtNum`, `dataId`), is stored as follows. With `e2sar_ft`, one event is one file.

```
<prefix>/<evtNum>/<dataId>/0      bytes [0, 1 MiB)
<prefix>/<evtNum>/<dataId>/1      bytes [1 MiB, 2 MiB)
...
<prefix>/<evtNum>/<dataId>/<n-1>  remaining bytes (<= 1 MiB)
<prefix>/<evtNum>/<dataId>        "size=<bytes> chunk=<bytes> nchunks=<n>"   <- written last
```

Chunk `i` covers bytes `[i*chunk, min((i+1)*chunk, size))`.

- **Not a file:** in KV mode an event is a group of KV values, not a file. `e2sar_daos_read --fetch -o DIR` joins the chunks back into one file (see [Reading a run back](#reading-a-run-back)).
- **Write granularity:**
    - Each event is split into chunks of `--chunk` KiB (default 1024 = 1 MiB).
    - Each chunk is one async `daos_kv_put`, so one KV value.
    - An event smaller than the chunk size is a single value, and the metadata records the event size as the chunk size. `--chunk 0` stores every event as a single value.
- **Commit marker:** the metadata key is written only after all of an event's chunks have been stored successfully. An event that is listed with metadata is complete. Chunks without metadata come from a failed or interrupted write. `e2sar_daos_read` reports them as *incomplete*.
- **Key prefix:** `e2sar_perf` and `e2sar_ft` number events from 0 on every run, so use a new `--prefix` for each run (e.g. `run42`). Otherwise new data overwrites old data under the same keys. A receiver that already wrote to a prefix warns when it starts again with it.
- **Key parts:** `prefix` is the run, `evtNum` is the E2SAR event number, and `dataId` is the sender stream. A Segmenter sends with its own dataId, so events from different streams with the same number don't collide.
- **Placement:** the object is hashed, so DAOS places each key on a target by its hash, according to the object class (`--oclass`, e.g. `SX` or `RP_2G1`). The chunks of one event usually land on different targets and are written in parallel. Keys have no order in DAOS: `e2sar_daos_read` lists all of the run's keys and groups them into events.

### Example: an EVIO file sent with `e2sar_ft`

`e2sar_ft` sends each file as one event, numbered 0, 1, 2, … in the order it queues them.
Say `frames_thread0_file0020.evio` (1,776,313,996 bytes) is the first file of a run, received with `--prefix run42` and sent with `--dataid 4321`.
1,776,313,996 bytes = 1,694 chunks of 1 MiB + 26,252 bytes, so the event takes 1,695 chunk values and one metadata value:

```
run42/0/4321/0       bytes [0, 1048576)
run42/0/4321/1       bytes [1048576, 2097152)
...
run42/0/4321/1693    bytes [1775239168, 1776287744)
run42/0/4321/1694    bytes [1776287744, 1776313996)                  26,252 bytes
run42/0/4321         "size=1776313996 chunk=1048576 nchunks=1695"    written last
```

The next file of the run is `run42/1/4321/...`, and so on.
`e2sar_daos_read --prefix run42 --fetch -o DIR` joins the chunks back into `DIR/run42_0_4321.evio`, byte for byte the original file.

### Example: events from `e2sar_evio_send`

Here one event is one EVIO v6 record holding `-f` built time frames, not a file.
Segmenter i sends with dataId 4321 + i and numbers its events 1…N, so every Segmenter has an event 1, and the dataId tells them apart.
[`scripts/e2sar_e2e_bench.sh`](../scripts/e2sar_e2e_bench.sh) passes its TAG as `--prefix`. With the bench defaults (16 Segmenters, 256 frames ≈ 95 KB per event), every event fits in one chunk. For a run tagged `tput20`:

```
tput20/1/4321/0      event 1 of Segmenter 0: the whole EVIO record (S ≈ 95 KB)
tput20/1/4321        "size=S chunk=S nchunks=1"
tput20/1/4322/0      event 1 of Segmenter 1
tput20/1/4322
...
tput20/1/4336/0      event 1 of Segmenter 15
tput20/1/4336
tput20/2/4321/0      event 2 of Segmenter 0
tput20/2/4321
...
```

S differs a little from event to event, with the hits in the frames.
With `-f 4096` (~1.5 MB events), each event has 2 chunks: `/0` with 1 MiB and `/1` with the remaining ~0.5 MB.

| | `e2sar_ft` | `e2sar_evio_send` |
|---|---|---|
| One event | One whole file (often GBs) | One EVIO record of `-f` frames (~95 KB by default) |
| `evtNum` | 0, 1, 2, … per run | 1…N per Segmenter |
| `dataId` | `--dataid`, one stream | 4321 + i, one per Segmenter |
| Chunks per event | Many (1,695 for the 1.7 GB file above) | Usually 1 |
| Restored by `e2sar_daos_read -o` | The original `.evio` file | One standalone EVIO v6 record |

## Reading a run back

```bash
e2sar_daos_read --cont <label> --prefixes                                  # list runs in the catalog
e2sar_daos_read --cont <label> --prefix run42                              # list committed events
e2sar_daos_read --cont <label> --prefix run42 --fetch                      # verify, print SHA-256
e2sar_daos_read --cont <label> --prefix run42 --fetch -o ~/restored        # write run42_<evt>_<dataId>.evio
e2sar_daos_read --cont <label> --prefix run42 --fetch -o ~/restored --event 7 --dataid 4321
e2sar_daos_read --cont <label> --prefix run42 --fetch -o ~/restored --bundle 100000  # 100,000 events per file
e2sar_daos_read --cont <label> --prefix run42 --delete                     # delete the run's object and catalog entries
```

To restore one event, the reader looks up `run/<prefix>` in the catalog, opens the run object, reads the metadata key, then fetches chunks `0..nchunks-1` in order and writes them to `DIR/<prefix>_<evtNum>_<dataId>.evio` (`-e` sets the extension, `-e ''` for none).
Events are read in event-number order, and by data id within one event number.
For many small events, such as one streaming frame per event, `--bundle N` writes N events per file (`<prefix>_000000.evio`, …) with an index per file (`evtNum dataId offset size`) and `<prefix>_missing.txt` listing the missing event numbers per data id. See [bin/README.md](../bin/README.md#e2sar_daos_read).
It opens the pool and container read-only, except with `--delete`. All options are in [bin/README.md](../bin/README.md#e2sar_daos_read).

## Shared object (before per-run objects)

Before per-run objects, everything went into one shared object, `--oid` (default `0xE25A`), with the same keys and no catalog.

- `e2sar_daos_recv --oid N` still writes that way.
- `e2sar_daos_read` reads a prefix that isn't in the catalog from that object, with a note on stderr, so older containers still read. `--oid` and `--oclass` select a different shared object.
- [E2E_BENCHMARK.md](E2E_BENCHMARK.md) runs used this layout.

## dfuse mode

With `e2sar_daos_recv --dfuse DIR` (the bench's `-S dfuse`), there is no KV object and no run catalog. Each event is one file in a POSIX container mounted with dfuse:

```
DIR/<prefix>/<prefix>_<evtNum>_<dataId>.evio
```

- Each `--deq` thread writes with a blocking `open` / `write` / `close`. No chunking: `--chunk` does not apply.
- An event counts as committed once `close()` returns. With `--fsync`, also after `fsync()`.
- The file name is the same as `e2sar_daos_read -o` produces in KV mode, so both can be checked the same way (e.g. `sha256sum`).

See [DFUSE_TESTING.md](DFUSE_TESTING.md) for setup and results.
