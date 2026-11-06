---
type: Reference
title: "e2sar-daos scripts"
description: "The bash and Python scripts that drive the apps on the JLab test nodes: the e2e bench, its rate sweep, the layout sweep and the site receiver."
tags: [scripts, e2sar_e2e_bench, e2sar_site2ebpf_recv]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/scripts/README.md", title: "scripts/README.md at 0d85d00" }
---

# e2sar-daos scripts

Bash and Python scripts that drive the [applications](../../bin/README.md) for tests on the JLab ebpf nodes. Each one prints its usage when run without arguments, and the comment at its top lists its settings.

| Script | Where it runs | What it does |
|---|---|---|
| [`e2sar_e2e_bench.sh`](../../scripts/e2sar_e2e_bench.sh) | Receiver and sender node, one role each | One end-to-end run: `e2sar_evio_send` (the frames of an EVIO file, `-F`, one per event unless `SEND_ARGS="-f F"`) → [LB] → `e2sar_daos_recv` → DAOS. Throughput, latency per stage, and a digest check of every event. See [e2e-bench](../howto/e2e-bench.md) |
| [`e2sar_e2e_sweep.sh`](../../scripts/e2sar_e2e_sweep.sh) | A machine that can SSH to both nodes | A rate sweep: runs `e2sar_e2e_bench.sh` on both nodes for each rate and repetition. See [2026-10-20-e2e-n2n-sweep section 7](../results/2026-10-20-e2e-n2n-sweep.md#7-node-to-node-rate-sweep-2050-gbps-2026-10-20) |
| [`e2sar_layout_sweep.py`](../../scripts/e2sar_layout_sweep.py) | A machine that can SSH to both nodes | The tuning sweep of `e2sar_daos_recv` (reassembly and dequeue threads, in-flight operations, batch flush) for the scenarios of [2026-10-29-receiver-tuning](../results/2026-10-29-receiver-tuning.md#tuning), one JSON line per run |
| [`e2sar_site2ebpf_recv.sh`](../../scripts/e2sar_site2ebpf_recv.sh) | ebpf2201 and ebpf2203 | Runs one node as a receiver for a remote sender (Perlmutter, or another site; `SITE` only picks the log directory): pre-flight checks, shared run directory, `e2sar_daos_recv` (with `-v`) into a new container `<CONT_LABEL>_<UTC epoch s>` per run, drop checks after Ctrl-C, then `verify N`, `runs` (the runs in DAOS, from the run catalog) and `clean [RUNID]` (destroy one run's container, or every `<CONT_LABEL>_<epoch s>` container). See [perlmutter](../sites/perlmutter.md#the-receiver-script) |
| [`e2sar_live_pipeline.sh`](../../scripts/e2sar_live_pipeline.sh) | A machine that can SSH to every node (the laptop) | The live pipeline of [streaming-workflow](../concepts/streaming-workflow.md#the-workflow) for a remote sender through an LB. `start`: a new container `LABEL_<UTC epoch s>`, on each receiver node `e2sar_nicmon.sh` then `e2sar_daos_recv -c`, on the reader node the DAOS monitor and its page, `e2sar_daos_read --live $READ_ARGS` (`--min-free 50`: the reader pauses below 50 GiB free, see [e2sar_daos_read](e2sar_daos_read.md#a-full-disk---min-free)) and the EVIO viewer. `stop`: receivers (they drain), reader, monitor, pages, NIC monitors; data kept, logs to `LOGDIR/<run>/`. `status`: processes, LB senders and workers, events, reader files, missing, NIC report, free space, and `READER PAUSED` when the reader is waiting for space. `recycle`: if the run is at least `IDLE_S` (3600 s) old and has no event at all, delete it (container, empty folder, monitor file; logs kept in `LOGDIR/empty_<run>/`) and start a new one; a run with any event is never deleted. `watch`: `status` and `recycle` every `EVERY_S` (3600 s), warning below `MIN_FREE_GB` (150). Never frees the LB. See [live-monitoring](../howto/live-monitoring.md#waiting-for-a-sender) |
| [`e2sar_nicmon.sh`](../../scripts/e2sar_nicmon.sh) | Every receiver node, for every run | Samples the NIC (`ethtool -S`: packets at the port and to the host, discards, out of buffer, steer-missed), IP, UDP and TCP counters (`/proc/net/snmp`, read by name) and softnet drops once a second: `start DIR` (snapshot, then a background sampler into `DIR/nic_<host>.log`), `stop DIR` (stop, snapshot), `report DIR` (each drop counter's change, the seconds in which one moved, and the seconds in which more than `GAP` packets (3000) reached the port but not a UDP socket). `e2sar_e2e_bench.sh` and `e2sar_site2ebpf_recv.sh` run it around the receiver. No root needed. On the ebpf nodes the data port also carries NFS (`$HOME`) and SSH, so check `Tcp.InSegs` before calling a gap a loss |
| [`evio/evio_concat.py`](../../scripts/evio/evio_concat.py) | Anywhere with Python 3 (standard library only) | `OUT.evio IN...`: joins EVIO v6 files and the reader's `.dat` files (bare records, no file header) into one EVIO v6 file: one file header (the first input's, or a new one if it has none), all records in order, the last-record bit only on the last. The only step that makes a `.evio` file from the reader's output. `--fix-last-bit FILE` fixes that bit in place. See [streaming-workflow](../concepts/streaming-workflow.md#the-workflow) |
| [`evio/evio_hit_compression.py`](../../scripts/evio/evio_hit_compression.py) | Anywhere with numpy and pyarrow (evio-interactive-view's `.venv`) | Reads the first N frames (20,000) of an EVIO v6 file or `.dat` bundle of fADC250 time frames and prints how far they compress, and how fast on one thread: lz4 and zstd on the raw bytes and per frame, Parquet (three layouts), and hit-aware columns (time step within a payload bank, channel, charge, count). Also the entropy of each hit-word field. `--tmp DIR` for the Parquet test files (removed after). See [hit-compression](../concepts/hit-compression.md) |
| [`utc.py`](../../scripts/utc.py) | Anywhere with Python ≥ 3.6 (standard library only) | Turns UTC epoch times into dates: seconds (`1793817098 --> UTC 2026-11-04 18:31:38`), log timestamps with a fraction, ms/µs/ns by digit count (e.g. the LB's `predictedeventnum`), the seconds in run and bundle names, and DAOS object ids `hi.lo` (kind, slot, second; see [daos-layout](../concepts/daos-layout.md#object-ids)). With no arguments it reads lines from stdin and adds the date after each time, e.g. `tail recv.log \| scripts/utc.py`. `-h` for usage, `--local` adds local time |

## Before running them

- **Build first.** The scripts run the apps from the build next to them, `../build/bin/` (`meson compile -C build`). Set `E2SAR_DAOS_BUILD` to use another build. Run the copy of a script in the checkout whose build you want, e.g. in a separate worktree.
- **Environment.** They read `~/.bashrc` for `E2SAR_BIN` (the E2SAR tools, such as `lbadm`) and, through the LB, the instance URI (`EJFAT_URI`). `e2sar_site2ebpf_recv.sh` takes the URI as `EJFAT_URI` (passed in or exported) and its listen address from `MY_IP`. See the [E2SAR environment](../howto/install-e2sar.md#the-e2sar-environment-bashrc) and, for the LB, [lb-test step 1](../howto/lb-test.md#1-environment-bashrc-and-the-lb-instance-once).
- **Settings.** Every setting is an environment variable with a default for the ebpf testbed, listed at the top of the script, e.g. `DEQ=16 scripts/e2sar_site2ebpf_recv.sh start`.
- **Shared `$HOME`.** The two ebpf nodes can't SSH to each other. The roles meet through files in `$RUN`, under the NFS `$HOME` both nodes share.
- **bash.** The ebpf login shell is tcsh: start `bash` first, or run a script as `bash scripts/<name>.sh`.

## Examples

```bash
# end-to-end bench through the LB: receiver first, then the sender, with the same TAG
bash ~/daq/e2sar-daos/scripts/e2sar_e2e_bench.sh recv run1 -m lb      # on ebpf2203
bash ~/daq/e2sar-daos/scripts/e2sar_e2e_bench.sh send run1 -m lb      # on ebpf2201

# a node-to-node rate sweep, 3 repetitions per rate, from a laptop
scripts/e2sar_e2e_sweep.sh 1020a "20 25 30 35 40 45 50" 3

# the Perlmutter receivers: ebpf2201 first, then ebpf2203
~/daq/e2sar-daos/scripts/e2sar_site2ebpf_recv.sh start
```

The analysis step of the bench, [`bin/e2sar_e2e_analyze.py`](../../bin/e2sar_e2e_analyze.py), stays in `bin/` next to the apps, and `e2sar_e2e_bench.sh` calls it from there.
