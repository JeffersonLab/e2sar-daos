# e2sar-daos applications

| App | What it does |
|---|---|
| `e2sar_daos_recv` | Receives events from an E2SAR sender (e.g. `e2sar_perf -s` or `e2sar_ft -s`) and writes them to DAOS: small events batched into 1 MiB chunks, large ones written directly, all asynchronously. One run per container |
| `e2sar_daos_read` | Reads a run back: following it while it is written (`--live`, `--live-rm`), or one file per event (`--offline`); also lists, verifies and deletes runs |
| `e2sar_evio_send` | Test sender: reads an EVIO v6 file (`-F`) and sends it by frames (each record is one frame, sent as one event) through several E2SAR Segmenters, paced, with per-event timing and digests. See [E2E_BENCHMARK.md](../docs/E2E_BENCHMARK.md#the-events) |
| `e2sar_e2e_analyze.py` | The analysis step of [`scripts/e2sar_e2e_bench.sh`](../scripts/e2sar_e2e_bench.sh). `--profile 0.1` shows latency and loss per 0.1 s of a run |

`e2sar_ft` sends each file as one E2SAR event. So an event read back from DAOS is byte-for-byte the original file, e.g. an EVIO v6 file you can open in an EVIO viewer.

The bash scripts that drive these apps on the test nodes (the end-to-end bench, its rate sweep, and the LCF receiver) are in [scripts/](../scripts/README.md).

The C++ apps are built by `meson compile -C build` and placed in `build/bin/`. `e2sar_daos_read` and `e2sar_evio_send` need `openssl-devel` for SHA-256.

## Prerequisites

- A running `daos_agent` on the receiving node, and access to the pool (default `sc26`).
- One run is one container. `e2sar_daos_recv` creates it on first use; `--cont-name` is its label, required unless `--dryrun`. [`e2sar_site2ebpf_recv.sh`](../scripts/e2sar_site2ebpf_recv.sh) makes one per run, `<CONT_LABEL>_<UTC epoch s>`, e.g. `e2sar_1793280000`. Pass the same label to `e2sar_daos_read --cont`.
- Without a control plane (no `--withcp`), the sender sends straight to the `data=` address in `EJFAT_URI`. The port there defaults to 19522, so start the receiver with `--port 19522`, or put an explicit port in the URI, e.g. `data=<ip>:10000`.

## Data layout in DAOS

A container holds one run: a catalog (run settings, one writer slot per receiver, their watermarks and final statistics), the event bytes in byte arrays `data[T, slot]`, and per UTC second an index `index[T]` of the batches committed in it. See [docs/DAOS_LAYOUT.md](../docs/DAOS_LAYOUT.md) for the layout and [docs/DAOS_READER.md](../docs/DAOS_READER.md) for reading.

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

## `e2sar_daos_read`

See [docs/DAOS_READER.md](../docs/DAOS_READER.md). Give exactly one of:

| Mode | Output in `-o DIR` |
|---|---|
| `--live` | Follows the run: one file per `--interval` s (default 5), the events glued back to back in `(evtNum, dataId)` order, with an `.idx` per file, a manifest (the restart point) and the missing event numbers. DAOS is left as it is |
| `--live-rm` | The same, and removes data from DAOS `--rm-after` intervals (default 10) behind the last file written; destroys the container at the end of the run |
| `--offline` | One file per `(evtNum, dataId)`, byte for byte the sent event, with its SHA-256 in `<run>.sha256`. DAOS is left as it is |
| `--list` | (no files) The run record, the writers, then one line per event: `evtNum dataId bytes second slot` |
| `--fetch` | (no files) Every event's SHA-256: `<sha256>  <run>_<evtNum>_<dataId>.evio  <bytes> bytes` |
| `--delete` | (no files) Destroy the run's container |

`--pool` and `--cont` are required. Start the writers first: a reader whose container doesn't exist yet stops with `container not found: start the writers first`.

## End-to-end example (no control plane)

On the receiver node (`192.168.1.20`), with DAOS:

```bash
export EJFAT_URI='ejfat://useless@10.0.0.1:18347/lb/1?data=192.168.1.20&sync=192.168.1.20:19020'
./build/bin/e2sar_daos_recv --ip 192.168.1.20 --port 19522 --pool sc26 --cont-name run42
```

On the sender node, using the same `EJFAT_URI`, send 100 events of 1 MiB at 5 Gbps with `e2sar_perf`:

```bash
e2sar_perf -s --ip <sender ip> --mtu 9000 -l 1048576 -n 100 --rate 5
```

- `-l` is the event size in bytes, `-n` the number of events and `--rate` the send rate in Gbps. Use `--mtu 1500` if the path doesn't pass 9000-byte packets.
- `e2sar_perf` numbers the events 0 to N−1 on every send, so use a new container (`--cont-name`) for each send.
- Every event is the same buffer: `This is a start of event payload` at its start and `...the end` at its end. Don't pass `--realmalloc`, which breaks the single-digest check below.

Stop the receiver with Ctrl-C once the sender finishes. Then check the data on any DAOS client node:

```bash
./build/bin/e2sar_daos_read --pool sc26 --cont run42 --list | tail -1     # 100 committed events, 104857600 bytes
./build/bin/e2sar_daos_read --pool sc26 --cont run42 --fetch | grep -E '^[0-9a-f]{64} ' | cut -d' ' -f1 | sort | uniq -c
#   100 <digest>          one line: all 100 events are the same buffer, intact
```

Fewer than 100 events means some were lost on the way (the receiver's `Lost in reassembly` shows how many); more than one digest means an event was corrupted. To look inside one event:

```bash
./build/bin/e2sar_daos_read --pool sc26 --cont run42 -o ~/restored --offline --event 0   # ~/restored/run42_0_<dataId>.evio
head -c 32 ~/restored/run42_0_*.evio; echo                 # This is a start of event payload
```

To store real files, one event per file (e.g. EVIO files you can open in a viewer), send them with `e2sar_ft -s --ip <sender ip> --rate 5 /path/to/files/*.evio`, then restore them with `--offline` and compare `sha256sum` of the originals with `~/restored/run42.sha256`.

Clean up after every test run, once it is verified:

```bash
rm -rf ~/restored
./build/bin/e2sar_daos_read --pool sc26 --cont run42 --delete
```

## Viewing the restored files

Events sent from EVIO files with `e2sar_ft` restore as ordinary `.evio` files, so any EVIO viewer opens them. (`e2sar_perf` events are not EVIO.) They are on the node where `e2sar_daos_read -o` ran, usually a remote DAOS client. To browse them from your laptop, run the [evio-interactive-view](https://code.jlab.org/xmei/evio-interactive-view) web viewer on that node and forward its port over SSH. See its README for setup (a Python venv with `pip install -r requirements.txt`).

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
