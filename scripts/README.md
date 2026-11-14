# e2sar-daos scripts

Bash and Python scripts that drive the [applications](../bin/README.md) on the JLab ebpf nodes. Each one prints its usage when run without arguments, and the comment at its top lists its settings.

| Script | What it does |
|---|---|
| [`e2sar_e2e_bench.sh`](e2sar_e2e_bench.sh) | One end-to-end run: `e2sar_evio_send` → [LB] → `e2sar_daos_recv` → DAOS, with throughput, latency per stage and a digest check |
| [`e2sar_e2e_sweep.sh`](e2sar_e2e_sweep.sh) | A rate sweep of `e2sar_e2e_bench.sh` from a machine that can SSH to both nodes |
| [`e2sar_layout_sweep.py`](e2sar_layout_sweep.py) | The tuning sweep of `e2sar_daos_recv`, one JSON line per run |
| [`e2sar_site2ebpf_recv.sh`](e2sar_site2ebpf_recv.sh) | Runs an ebpf node as a receiver for a remote sender (Perlmutter or another site) |
| [`e2sar_live_pipeline.sh`](e2sar_live_pipeline.sh) | The live pipeline for a remote sender through an LB, from a machine that can SSH to every node: `start` (checks the LB instance URI, names the run `e2sar_lb<LB id>_<UTC epoch s>`), `stop`, `status`, `recycle` (replace a run that got no events for an hour) and `watch` (status and recycle every hour) |
| [`e2sar_workflow.sh`](e2sar_workflow.sh) | The same live workflow, run on the ebpf nodes one by one (no SSH between them): `start`, `status`, `forward`, `list`, `evio N\|FILE.dat\|all` (bundles into an EVIO file for someone else), `stop` |
| [`e2sar_nicmon.sh`](e2sar_nicmon.sh) | 1 s samples of a receiver node's NIC, IP, UDP and TCP counters during a run: `start`, `stop`, `report` |
| [`e2sar_loss_timeline.py`](e2sar_loss_timeline.py) | A run's receiver losses per minute, by cause matched from the `e2sar_nicmon.sh` samples (NIC port discard, kernel IP→UDP drop, upstream, ...), with the cumulative loss rate: a PNG and a table |
| [`capture/`](capture/README.md) | Screenshots for the docs from the laptop: `shot_monitor.js` (the DAOS monitor, light theme on white) |
| [`evio/evio_concat.py`](evio/evio_concat.py) | Joins EVIO v6 files and the reader's `.dat` files (bare records) into one EVIO v6 file, adding the file header: the step that makes a `.evio` file |
| [`evio/evio_hit_compression.py`](evio/evio_hit_compression.py) | How far fADC250 time frames compress, by method |
| [`utc.py`](utc.py) | Turns the UTC epoch times in run names, bundle names, DAOS object ids and log lines into dates (`-h` for usage; with no arguments it reads lines from stdin) |

Where each runs, the environment they need, and examples: [docs/reference/scripts.md](../docs/reference/scripts.md).
