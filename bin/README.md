# e2sar-daos applications

| App | What it does |
|---|---|
| `e2sar_daos_recv` | Receives events from an E2SAR sender (e.g. `e2sar_ft -s`) and writes them to a DAOS KV object with asynchronous puts |
| `e2sar_daos_read` | Lists what `e2sar_daos_recv` wrote, verifies it (sizes, SHA-256, EVIO header), and restores events as `.evio` files |
| `e2sar_evio_send` | Test sender: synthetic EVIO v6 streaming time frames through several E2SAR Segmenters, paced, with per-event timing and digests |
| `e2sar_e2e_bench.sh` | One end-to-end run (`e2sar_evio_send` → [LB] → `e2sar_daos_recv` → DAOS): throughput, latency per stage, full digest check. See [docs/E2E_BENCHMARK.md](../docs/E2E_BENCHMARK.md) |
| `e2sar_e2e_analyze.py` | The analysis step of `e2sar_e2e_bench.sh`. `--profile 0.1` shows latency and loss per 0.1 s of a run |
| `e2sar_e2e_sweep.sh` | A rate sweep of `e2sar_e2e_bench.sh`, run from a machine that can SSH to both nodes. See [docs/E2E_BENCHMARK.md section 7](../docs/E2E_BENCHMARK.md#7-node-to-node-rate-sweep-2050-gbps-2026-10-20) |
| [`docs/data/e2sar_e2e_sweep_report.py`](../docs/data/e2sar_e2e_sweep_report.py) | Summarizes a sweep's `.jsonl` (one result per run) as Markdown tables per sweep and rate: loss, throughput, latency per stage, warm-up. `--csv` writes one flat row per run. E.g. `docs/data/e2sar_e2e_sweep_report.py docs/data/e2e_sweep_2026-10-20.jsonl --merge` |

`e2sar_ft` sends each file as one E2SAR event. So an event read back from DAOS is byte-for-byte the original file, e.g. an EVIO v6 file you can open in an EVIO viewer.

The C++ apps are built by `meson compile -C build` and placed in `build/bin/`. `e2sar_daos_read` and `e2sar_evio_send` need `openssl-devel` for SHA-256.

## Prerequisites

- A running `daos_agent` on the receiving node, and access to the pool (default `sc26`).
- The container does not need to exist. `e2sar_daos_recv` creates it on first use (default label `e2sar`).
- Without a control plane (no `--withcp`), the sender sends straight to the `data=` address in `EJFAT_URI`. The port there defaults to 19522, so start the receiver with `--port 19522`, or put an explicit port in the URI, e.g. `data=<ip>:10000`.

## Data layout in DAOS

Everything goes into one KV object (`DAOS_OT_KV_HASHED`) in `<pool>/<cont>`, identified by `--oid` (default `0xE25A`):

```
pool                  --pool, e.g. sc26
└── container         --cont, e.g. e2sar (created on first use)
    └── KV object     DAOS_OT_KV_HASHED, OID low bits --oid (default 0xE25A), class --oclass
        └── keys      <prefix>/<evtNum>/<dataId>[/<chunk>], one KV value each
```

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
- **Key parts:** `prefix` is the run, `evtNum` is the E2SAR event number, and `dataId` is the sender stream. A Segmenter sends with its own dataId, so events from different streams with the same number don't collide.
- **Placement:** the object is hashed, so DAOS places each key on a target by its hash, according to the object class (`--oclass`, e.g. `SX` or `RP_2G1`). The chunks of one event usually land on different targets and are written in parallel. Keys have no order in DAOS: `e2sar_daos_read` lists them all and groups them into events.

The layout is defined in [`include/e2sar_daos/event_layout.hpp`](../include/e2sar_daos/event_layout.hpp).

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
`e2sar_e2e_bench.sh` passes its TAG as `--prefix`. With the bench defaults (16 Segmenters, 256 frames ≈ 95 KB per event), every event fits in one chunk. For a run tagged `tput20`:

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
When an event is smaller than the chunk size, the metadata records the event size as the chunk size.
With `-f 4096` (~1.5 MB events), each event has 2 chunks: `/0` with 1 MiB and `/1` with the remaining ~0.5 MB.

| | `e2sar_ft` | `e2sar_evio_send` |
|---|---|---|
| One event | One whole file (often GBs) | One EVIO record of `-f` frames (~95 KB by default) |
| `evtNum` | 0, 1, 2, … per run | 1…N per Segmenter |
| `dataId` | `--dataid`, one stream | 4321 + i, one per Segmenter |
| Chunks per event | Many (1,695 for the 1.7 GB file above) | Usually 1 |
| Restored by `e2sar_daos_read -o` | The original `.evio` file | One standalone EVIO v6 record |

With `e2sar_daos_recv --dfuse DIR` (the bench's `-S dfuse`), there is no KV object: each event is one file, `DIR/<prefix>/<prefix>_<evtNum>_<dataId>.evio`, in a POSIX container. See [docs/DFUSE_TESTING.md](../docs/DFUSE_TESTING.md).

## How writing works

```
Reassembler ──recvEvent──▶ dequeue thread 0 ──▶ AsyncKVWriter (own DAOS event queue, ≤ --inflight puts)
            ──recvEvent──▶ dequeue thread 1 ──▶ AsyncKVWriter (own DAOS event queue, ≤ --inflight puts)
                                                       └──────────── shared pool / container / KV handle
```

- Each dequeue thread (`--deq`) owns one DAOS event queue, because DAOS event queues must be polled by a single thread.
- A thread keeps up to `--inflight` puts outstanding (default 128, so 128 MiB per thread with 1 MiB chunks). It reaps completions between `recvEvent` calls.
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
| `--inflight` | 128 | Max concurrent puts per dequeue thread |
| `--deq` | 16 | Dequeue threads, each with its own event queue. Independent of `--threads`. Below 16, the receiver fell behind above ~25 Gbps ([E2E_BENCHMARK.md](../docs/E2E_BENCHMARK.md)) |
| `--report` | 1 | Progress line interval in seconds (0 = off) |
| `--verbose` | off | Print every committed event |
| `--timing-csv FILE` | — | Write one row of timing per committed event (see [Timing and throughput](#timing-and-throughput)) |

Synthetic mode (no sender, no network):

| Option | Default | |
|---|---|---|
| `--synthetic N` | 0 (off) | Store N events made in memory instead of receiving. The E2SAR options are ignored and `--ip` is not needed |
| `--event-size` | `95K` | Bytes per event, with an optional `K`/`M`/`G` suffix (binary), e.g. `1.5M` |
| `--rate` | 0 | Pace events to this many Gbps. 0 = as fast as the sink takes them |

Exit status: 0 if every received event was committed, 2 if any event failed, 1 on a setup error.

### Synthetic mode: the sink's ceiling without a sender

`--synthetic N` replaces the network and the Reassembler with an in-memory generator. Everything after that is the normal receiver: the same `--deq` threads, `--inflight` puts, `--chunk` layout, keys, `--dfuse` sink and `Timing:` block. The run ends by itself after the last event is committed.

```bash
# KV sink, 100,000 x 95 KiB events, as fast as DAOS takes them
e2sar_daos_recv --synthetic 100000 --event-size 95K --pool sc26 --cont e2sar-test --prefix synth1
# the same paced at 20 Gbps, to see latency at a given load
e2sar_daos_recv --synthetic 100000 --rate 20 --pool sc26 --cont e2sar-test --prefix synth2
# dfuse sink
e2sar_daos_recv --synthetic 50000 --dfuse /tmp/$USER-dfuse --prefix synth3
```

Unpaced, `DAOS commit throughput` is the most this sink commits with these settings. An end-to-end run with the same settings can't do better, so the gap between the two is what the network and reassembly cost. This is a closer reference than `fio --ioengine=dfs`, which writes byte ranges of a few large files rather than one KV put (or one file) per event.

- Each event is a new buffer, filled with a copy of a fixed pseudo-random pattern, like the Reassembler's copy of an incoming event. Its first 8 bytes are the event number (little-endian), and the data id is 0. `e2sar_daos_read` reads the events back as usual.
- `Ingest throughput` is the generation rate: how fast the dequeue threads took events. Unpaced, it only tracks the sink.
- The Reassembler counters and the progress line's loss counters are left out.
- With `--rate`, event *i* is released at *i* × size × 8 / rate after start. If the sink falls behind, events are released late, never dropped, and the commit throughput shows the shortfall.

### Timing and throughput

The progress line, every `--report` seconds, shows the rate over the last interval (all numbers in this section are illustrative, not measurements):
```
in 412 evts, 4.87 Gbps | committed 405 evts, 38120.55 MiB, 4.79 Gbps, 50.00 evt/s | failed 0 | reassembly loss 0, enqueue loss 0
```
`in` is what E2SAR handed over (network side). `committed` is what DAOS has fully stored.

At exit, after the Reassembler and DAOS counters, a `Timing:` block summarizes the run:
```
Timing:
	First event received:      3.214 s after start
	Receive window:            41.871 s (first -> last event received)
	Commit window:             41.903 s (first event received -> last event committed)
	Ingest throughput:         4.912 Gbps, 585.6 MiB/s, 49.9 evt/s
	DAOS commit throughput:    4.908 Gbps, 585.1 MiB/s, 49.9 evt/s
	Event latency (recv->commit): min 9.812 | mean 21.440 | p50 19.922 | p90 29.297 | p99 46.875 | max 61.203 ms  (2090)
	daos_kv_put latency:       min 0.912 | mean 4.311 | p50 3.906 | p90 6.836 | p99 11.719 | max 18.442 ms  (25080)
```

| Line | Meaning |
|---|---|
| Receive window | From the first event handed over to the last one. Idle time before the sender starts and after it stops is excluded, so stopping with Ctrl-C late doesn't matter |
| Commit window | From the first event received to the last event committed to DAOS |
| Ingest throughput | Bytes received / receive window: what the network + reassembly delivered |
| DAOS commit throughput | Bytes committed / commit window: **the end-to-end number to quote**. If it matches ingest, DAOS kept up and the sender's rate was the limit |
| Event latency | From an event being handed over to its metadata key being committed. Includes waiting for a free put slot, so it grows when DAOS is the bottleneck |
| `daos_kv_put` latency | Each chunk or metadata put, from launch to completion, i.e. DAOS's own response time at the current queue depth (`--deq` x `--inflight`) |

Notes:
- A window is measured between events, so the first event's own transfer time is not in it. With only a few events, throughput is overestimated. Send at least tens of events.
- Percentiles come from a log-scale histogram and are accurate to within 12.5%. Min, mean and max are exact.
- If ingest throughput is well below the sender's `--rate`, look at `reassembly loss` and `enqueue loss` first. If they are 0, the dequeue threads are probably behind: events wait in the Reassembler's queue, which the event latency above does not include (it starts when a dequeue thread takes the event). Try more `--deq` threads. Going from 4 to 16 raised the received rate from 24 to 40 Gbps ([E2E_BENCHMARK.md](../docs/E2E_BENCHMARK.md)).

`--timing-csv FILE` writes one row per committed event, with times in seconds since the program started:
```
evt_num,data_id,bytes,chunks,thread,recv_s,commit_s,latency_ms
0,4321,93456789,90,0,3.214001,3.236950,22.949000
```
For example, plot `commit_s` against the running sum of `bytes` for throughput over time, or a histogram of `latency_ms`.

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
./build/bin/e2sar_daos_recv --ip 192.168.1.20 --port 19522 --pool sc26 --cont e2sar --prefix run42
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

The restored files are ordinary EVIO files. To open them on a remote node, see [Viewing the restored files](#viewing-the-restored-files).

`e2sar_ft` sends the files of a run as events 0, 1, 2, ... in traversal order. With more than one enqueue thread (`--enq`), the order is not guaranteed. So compare digests as sets, not line by line:

```bash
diff <(sha256sum /path/to/files/*.evio | cut -d' ' -f1 | sort) \
     <(./build/bin/e2sar_daos_read --prefix run42 --fetch | grep -E '^[0-9a-f]{64} ' | cut -d' ' -f1 | sort)
```

Clean up after every test run, once it is verified:

```bash
rm -rf ~/restored                      # restored files (where -o wrote them)
daos cont destroy sc26 e2sar           # the run's event data; only if the container holds test data alone
rm -rf /path/to/staged/copies          # sender: only copies made for the test, never the original data
```
`daos cont destroy` removes every prefix in the container. If the container is shared with data you want to keep, use a separate container for tests (`--cont e2sar-test`) and destroy that one instead.

## Viewing the restored files

The restored `.evio` files are ordinary EVIO files, so any EVIO viewer opens them. They are on the node where `e2sar_daos_read -o` ran, usually a remote DAOS client. To browse them from your laptop, run the [evio-interactive-view](https://code.jlab.org/xmei/evio-interactive-view) web viewer on that node and forward its port over SSH. See its README for setup (a Python venv with `pip install -r requirements.txt`).

```bash
# on the DAOS client node, in the evio-interactive-view checkout
python serve.py ~/restored --no-browser
# on your laptop
ssh -L 8765:localhost:8765 user@daos-client
```

Then open http://localhost:8765 on your laptop and click a `run42_<evt>_<dataId>.evio` file in the folder browser.

- **Another port:** if port 8765 is taken on the node, start with `--port 9000` and forward `-L 8765:localhost:9000`.
- **Tunnel only:** the server listens on 127.0.0.1 only, so the SSH tunnel is the only way to reach it.
- **The file is sent to your laptop:** decoding runs in the browser, so the whole file goes through the tunnel.
- **Size limit:** files must be smaller than 2 GiB. Split larger events with its `evio_split.py` first.

Delete the restored files when you are done (see the cleanup above).
