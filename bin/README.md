# e2sar-daos applications

| App | What it does |
|---|---|
| `e2sar_daos_recv` | Receives events from an E2SAR sender (e.g. `e2sar_ft -s`) and writes them to a DAOS KV object with asynchronous puts |
| `e2sar_daos_read` | Lists what `e2sar_daos_recv` wrote, verifies it (sizes, SHA-256, EVIO header), and restores events as `.evio` files |

`e2sar_ft` sends each file as one E2SAR event. So an event read back from DAOS is byte-for-byte the original file, e.g. an EVIO v6 file you can open in an EVIO viewer.

Both are built by `meson compile -C build` and placed in `build/bin/`. `e2sar_daos_read` needs `openssl-devel` for SHA-256.

## Prerequisites

- A running `daos_agent` on the receiving node, and access to the pool (default `sc26`).
- The container does not need to exist. `e2sar_daos_recv` creates it on first use (default label `e2sar`).
- Without a control plane (no `--withcp`), the sender sends straight to the `data=` address in `EJFAT_URI`. The port there defaults to 19522, so start the receiver with `--port 19522`, or put an explicit port in the URI, e.g. `data=<ip>:10000`.

## Data layout in DAOS

Everything goes into one KV object (`DAOS_OT_KV_HASHED`) in `<pool>/<cont>`, identified by `--oid` (default `0xE25A`).
Each reassembled event is stored as follows. With `e2sar_ft`, one event is one file.

```
<prefix>/<evtNum>/<dataId>/0      bytes [0, 1 MiB)
<prefix>/<evtNum>/<dataId>/1      bytes [1 MiB, 2 MiB)
...
<prefix>/<evtNum>/<dataId>/<n-1>  remaining bytes (<= 1 MiB)
<prefix>/<evtNum>/<dataId>        "size=<bytes> chunk=<bytes> nchunks=<n>"   <- written last
```

- **Write granularity:**
    - Each event is split into chunks of `--chunk` KiB (default 1024 = 1 MiB).
    - Each chunk is one async `daos_kv_put`, so one KV value.
    - An event smaller than the chunk size is a single value. `--chunk 0` stores every event as a single value.
- **Commit marker:** the metadata key is written only after all of an event's chunks have been stored successfully. An event that is listed with metadata is complete. Chunks without metadata come from a failed or interrupted write. `e2sar_daos_read` reports them as *incomplete*.
- **Key prefix:** `e2sar_ft` numbers events from 0 on every run, so use a new `--prefix` for each run (e.g. `run42`). Otherwise new data overwrites old data under the same keys.

The layout is defined in [`include/e2sar_daos/event_layout.hpp`](../include/e2sar_daos/event_layout.hpp).

## How writing works

```
Reassembler ──recvEvent──▶ dequeue thread 0 ──▶ AsyncKVWriter (own DAOS event queue, ≤ --inflight puts)
            ──recvEvent──▶ dequeue thread 1 ──▶ AsyncKVWriter (own DAOS event queue, ≤ --inflight puts)
                                                       └──────────── shared pool / container / KV handle
```

- Each dequeue thread (`--deq`) owns one DAOS event queue, because DAOS event queues must be polled by a single thread.
- A thread keeps up to `--inflight` puts outstanding (default 64, so 64 MiB per thread with 1 MiB chunks). It reaps completions between `recvEvent` calls.
- The event buffer is freed once all of its chunks have completed.
- When all slots are busy, the thread blocks on the event queue. That is the back-pressure: if DAOS can't keep up, the Reassembler queue fills, and the loss shows up as *enqueue loss* in the progress line.
- On Ctrl-C the threads stop receiving and drain every outstanding put, including pending metadata commits. Then the worker is deregistered and the final stats are printed.

## `e2sar_daos_recv`

```bash
e2sar_daos_recv -u '<EJFAT_URI>' --ip <listen ip> --prefix run42 [options]
```

E2SAR options (same meaning as `e2sar_ft -r`):

| Option | Default | |
|---|---|---|
| `-u, --uri` | `$EJFAT_URI` | EJFAT URI |
| `-c, --withcp` | off | Use the control plane (register worker with the LB) |
| `--ip` / `--autoip` | — | Listen address (exactly one is required) |
| `--port` | 10000 | Starting UDP port |
| `--threads` / `--cores` | 1 | E2SAR receive threads, or cores to pin them to |
| `-b, --bufsize` | 3 MB | Socket receive buffer |
| `--timeout` | 500 | Event reassembly timeout (ms) |
| `--numa` | -1 | Bind memory to this NUMA node |
| `-4`, `-6`, `-v` | | Prefer IPv4/IPv6 for the control plane; skip cert validation |

DAOS options:

| Option | Default | |
|---|---|---|
| `--pool` | `sc26` | Pool label (must exist) |
| `--cont` | `e2sar` | Container label (created if missing) |
| `--oid` | `0xE25A` | Low 64 bits of the KV object id |
| `--oclass` | container default | Object class, e.g. `SX`, `RP_2G1` |
| `--prefix` | `run` | Key prefix, one per run |
| `--chunk` | 1024 | Chunk size in KiB (0 = whole event in one value) |
| `--inflight` | 64 | Max concurrent puts per dequeue thread |
| `--deq` | 1 | Dequeue threads, each with its own event queue |
| `--report` | 1 | Progress line interval in seconds (0 = off) |
| `--verbose` | off | Print every committed event |

Exit status: 0 if every received event was committed, 2 if any event failed, 1 on a setup error.

## `e2sar_daos_read`

| Option | Default | |
|---|---|---|
| `--prefix` | (required) | Prefix given to `e2sar_daos_recv` |
| `--pool`, `--cont`, `--oid`, `--oclass` | same as recv | Must match what the receiver used |
| `--fetch` | off | Read events back and print `sha256sum`-style lines |
| `--event N ...` | all | Only these event numbers |
| `--dataid D` | all | Only this data id |
| `-o, --out DIR` | — | With `--fetch`, also write each event to `DIR/<prefix>_<evtNum>_<dataId>.evio` |
| `-e, --extension` | `evio` | Extension for `--out` files (`-e ''` for none) |

Without `--fetch` it lists committed events (number, data id, size, chunks) and any incomplete ones.
With `--fetch` it prints one line per event: the SHA-256, the file name, the size and chunk count, and the format found in the first bytes. The format is `EVIO v6 big-endian` (or `v4`, or `little-endian`) when the EVIO magic word `0xc0da0100` is at word 7, and `not EVIO` otherwise.
It opens the pool and container read-only and never creates anything.

## End-to-end example (no control plane)

On the receiver node (`192.168.1.20`), with DAOS:

```bash
export EJFAT_URI='ejfat://useless@10.0.0.1:18347/lb/1?data=192.168.1.20&sync=192.168.1.20:19020'
./build/bin/e2sar_daos_recv --ip 192.168.1.20 --port 19522 --pool sc26 --cont e2sar --prefix run42 --deq 2
```

On the sender node, using the same `EJFAT_URI`:

```bash
e2sar_ft -s --ip <sender ip> --rate 5 /path/to/files/*.evio
sha256sum /path/to/files/*.evio
```

Stop the receiver with Ctrl-C once the sender finishes. Then check the data on any DAOS client node:

```bash
./build/bin/e2sar_daos_read --prefix run42                 # list
./build/bin/e2sar_daos_read --prefix run42 --fetch         # sha256 per event
./build/bin/e2sar_daos_read --prefix run42 --fetch -o ~/restored   # run42_<evt>_<dataId>.evio
```

The restored `.evio` files open directly in an EVIO viewer, e.g. evio-interactive-view: `python serve.py ~/restored`.

`e2sar_ft` sends the files of a run as events 0, 1, 2, ... in traversal order. With more than one enqueue thread (`--enq`), the order is not guaranteed. So compare digests as sets, not line by line:

```bash
diff <(sha256sum /path/to/files/*.evio | cut -d' ' -f1 | sort) \
     <(./build/bin/e2sar_daos_read --prefix run42 --fetch | grep -E '^[0-9a-f]{64} ' | cut -d' ' -f1 | sort)
```
