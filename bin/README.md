# e2sar-daos applications

| App | What it does |
|---|---|
| `e2sar_daos_recv` | Receives events from an E2SAR sender (e.g. `e2sar_perf -s` or `e2sar_ft -s`) and writes them to DAOS: small events batched into 1 MiB chunks, large ones written directly, all asynchronously. One run per container |
| `e2sar_daos_read` | Reads a run back: following it while it is written (`--live`, `--live-rm`), or one file per event (`--offline`); also lists, verifies and deletes runs |
| `e2sar_daos_monitor` | Watches one run's container, read-only, and writes incoming, outgoing and missing events every 2 s as JSON lines, for the dashboard in [`viz/daos_monitor/`](../viz/daos_monitor/serve.py). See [e2sar_daos_monitor](../docs/reference/e2sar_daos_monitor.md) |
| `e2sar_evio_send` | Test sender: reads an EVIO v6 file (`-F`) and sends it by frames (each record is one frame; `-f F` consecutive frames per event, default 1) through several E2SAR Segmenters, paced, with per-event timing and digests. See [e2e-bench](../docs/howto/e2e-bench.md#the-events) |
| `e2sar_e2e_analyze.py` | The analysis step of [`scripts/e2sar_e2e_bench.sh`](../scripts/e2sar_e2e_bench.sh). `--profile 0.1` shows latency and loss per 0.1 s of a run |
| `e2sar_live_check.py` | Checks the files of `e2sar_daos_read --live`/`--live-rm` against the frames `e2sar_evio_send --digests` sent. See [the walkthrough](../docs/howto/streaming-walkthrough.md#walkthrough---live-rm-streaming-of-an-evio-file) |
| `evio_concat.py` | Joins EVIO v6 files into one valid EVIO v6 file (one file header, all records, the last-record bit only on the last). `--fix-last-bit FILE` fixes that bit in place |

`e2sar_ft` sends each file as one E2SAR event. So an event read back from DAOS is byte-for-byte the original file, e.g. an EVIO v6 file you can open in an EVIO viewer.

The bash scripts that drive these apps on the test nodes (the end-to-end bench, its rate sweep, the receiver tuning sweep, and the LCF receiver) are in [scripts/](../scripts/README.md).

The C++ apps are built by `meson compile -C build` and placed in `build/bin/`. `e2sar_daos_read` and `e2sar_evio_send` need `openssl-devel` for SHA-256.

## Documentation

| | |
|---|---|
| [docs/reference/e2sar_daos_recv.md](../docs/reference/e2sar_daos_recv.md) | `e2sar_daos_recv`: how writing works, every option, `--synthetic`, the progress line and the `Timing:` block |
| [docs/reference/e2sar_daos_read.md](../docs/reference/e2sar_daos_read.md) | `e2sar_daos_read`: the reading modes, what `--live-rm` removes, every option |
| [docs/concepts/daos-layout.md](../docs/concepts/daos-layout.md) | What a run looks like in DAOS, and the writer–reader contract |
| [docs/howto/e2e-no-control-plane.md](../docs/howto/e2e-no-control-plane.md) | A first end-to-end run without a control plane, and viewing restored EVIO files |
| [docs/howto/e2e-bench.md](../docs/howto/e2e-bench.md) | `e2sar_evio_send` and the end-to-end bench |
| [docs/reference/e2sar_daos_monitor.md](../docs/reference/e2sar_daos_monitor.md) | `e2sar_daos_monitor` and its live/replay dashboard |
| [docs/howto/live-monitoring.md](../docs/howto/live-monitoring.md) | The DAOS monitor and the EVIO live viewer on a laptop, through SSH |

## `e2sar_daos_read`

All options and what each mode writes: [docs/reference/e2sar_daos_read.md](../docs/reference/e2sar_daos_read.md). Give exactly one of:

| Mode | Output in `-o DIR` |
|---|---|
| `--live` | Follows the run: one file per `--interval` s (default 5), the events glued back to back in `(evtNum, dataId)` order, with an `.idx` per file, a manifest (the restart point) and the missing event numbers. DAOS is left as it is |
| `--live-rm` | The same, and removes data from DAOS `--rm-after` intervals (default 10) behind the last file written; destroys the container at the end of the run |
| `--offline` | One file per `(evtNum, dataId)`, byte for byte the sent event, with its SHA-256 in `<run>.sha256`. DAOS is left as it is |
| `--list` | (no files) The run record, the writers, then one line per event: `evtNum dataId bytes second slot` |
| `--fetch` | (no files) Every event's SHA-256: `<sha256>  <run>_<evtNum>_<dataId>.evio  <bytes> bytes` |
| `--delete` | (no files) Destroy the run's container |

`--pool` and `--cont` are required. Start the writers first: a reader whose container doesn't exist yet stops with `container not found: start the writers first`.
