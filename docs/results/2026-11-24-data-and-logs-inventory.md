---
type: Result
title: "Data and logs kept, as of 2026-11-24, and the analyses they allow"
description: "Every run's data and logs still on disk on 2026-11-24: one DAOS container (LB 686 40 Gbps, 10.10 TB in sc-demo), pool sc26 gone, 7 one-GiB EVIO samples, LB 682 reader bundles, monitor JSON lines for 13 runs, receiver/NIC/reader logs under ~/e2sar_run, the iperf3 nightly history, and the laptop bench folders. What is gone (LB 675/677 bundles, every sc26 container), what is unexplained (two 2026-11-17 EVIO samples, LB 682 run 1794639960), and a to-do list of analyses for each data set."
tags: [inventory, todo, analysis, daos, sc-demo, sc26, lb, ejfat, ebpf-testbed, logs, evio]
sources:
  - { id: summary, resource: "2026-11-12-rate-summary.md", title: "Rates and losses by setting" }
  - { id: e2e, resource: "e2e-summary.md", title: "End-to-end results: summary" }
---

# Data and logs kept, as of 2026-11-24, and the analyses they allow

Checked on 2026-11-24 from ebpf2201 and ebpf2203 (`daos pool list`, `daos cont list`, `ls`/`du` of `/scratch` and `~/e2sar_run`) and on the laptop. No receiver, reader, monitor page or hourly job was running on either node.

## Where everything is

| Where | What | Size |
|---|---|---|
| DAOS pool **sc-demo** (12 TB NVMe, 2.1 TB free, 82% used) | One container: `e2sar_lb686_1794515844` | 10.10 TB |
| DAOS pool **sc26** | **Gone.** Not in `daos pool list`; ranks 4 and 5 answer `DER_OOG` ("out of group"). Every container in it (LB 675, 677, 682 runs, the 2026-10 sweeps) is unreachable | – |
| ebpf2201 `/scratch/xmei` | LB 682 reader bundles (`live_e2sar_perlmutter_lb682_*`, 3 runs) and monitor JSON lines for LB 682 and LB 686 runs | 2.9 GB + ~15 MB |
| ebpf2201 `/scratch/epsdaq` (group `epsci`) | Three 1 GiB EVIO samples (LB 686) and monitor JSON lines for LB 675 and LB 677 | 3 GB + ~6 MB |
| ebpf2203 `/scratch/epsdaq` | Three 1 GiB EVIO samples (LB 686), `lb682_fromdaos/` (one 1 GiB EVIO file restored from DAOS, LB 682), two monitor JSON lines | 4 GB |
| `~/e2sar_run/` (shared `$HOME`, 287 MB) | Per-run logs: `lb677/`, `lb686/`, `perlmutter/` (LB 682), `lbbeta/` (LB 675), `pm/` (2026-10-27 Perlmutter sweep, per-event CSVs), `sweep/` (2026-10-29 tuning, 68 runs), `bench/` (one node-to-node E2E bench) | 287 MB |
| `~/daq/iperf3-nightly/` | `history.csv` (232 rows since 2026-10-20: UDP, TCP and TCP-max between the nodes, CPU, NIC drops), `runs/` per night | small |
| Laptop `~/Documents/DAQ/workflow-runs/` | Copies of the LB 686 1794515182 and 1794515844 logs, loss-by-cause plots and tables, monitor replay videos | 26 MB |
| Laptop `pm-bench/`, `lux-bench/`, `n2n-bench/` | Perlmutter 2026-10-27, LUX 2026-10-28/29 and node-to-node 2026-10-29 runs, with their parse and plot scripts | 263 / 83 / 12 MB |

## Run by run

✔ = on disk, – = gone. "Logs" means `recv_*.log`, `nic_*.log` (1 s NIC and UDP counters), `nic_before/after`, and where there was one, `read.log` and `monitor.log`, all in `~/e2sar_run/<dir>/<run>/`.

| Run | LB, sender | DAOS | Bundles / EVIO | Monitor JSON lines | Logs | Result page |
|---|---|---|---|---|---|---|
| `e2sar_lb675_1793817098` (2026-11-04) | 675, NERSC | – (sc26) | – | ✔ 2201 `/scratch/epsdaq` (full + cropped) | ✔ `lbbeta/` (flat, by name) | [2026-11-04-nersc-lb675-evio](2026-11-04-nersc-lb675-evio.md) |
| `e2sar_lb677_1793905825` run 1 | 677, LUX + NERSC | – (sc26) | – | ✔ 2201 `/scratch/epsdaq` | ✔ `lb677/` | [run 1](2026-11-05-194730UTC-lb677-run1-evio.md) |
| `e2sar_lb677_1793909535` run 2 | 677, LUX + NERSC | – (sc26) | – | ✔ 2201 `/scratch/epsdaq` | ✔ `lb677/` | [run 2](2026-11-05-201815UTC-lb677-run2-evio.md) |
| `e2sar_lb677_1793982005` (empty) | 677 | – (sc26) | – | ✔ 2203 `/scratch/epsdaq` | ✔ `lb677/` | – |
| 15 empty LB 677 runs | 677 | – | – | – | ✔ `lb677/empty_*` | – |
| `e2sar_perlmutter_lb682_1794356556`, `…1794360173`, `…1794396455` | 682, Perlmutter | – (sc26) | ✔ 2201 `/scratch/xmei/live_*` (0.75–1.2 GB each) | ✔ 2201 `/scratch/xmei` | ✔ `perlmutter/` | [LB 682 losses](2026-11-09-lb682-perlmutter-losses.md) |
| `e2sar_perlmutter_1794259253` | 682, Perlmutter | – (sc26) | ✔ 2203 `/scratch/epsdaq/lb682_fromdaos/` (1 GiB EVIO) | – | – | [LB 682 losses](2026-11-09-lb682-perlmutter-losses.md) |
| `e2sar_perlmutter_lb682_1794639960` (2026-11-14) | 682 (freed) | – | – | – | ✔ `perlmutter/` (recv log is 1.6 MB of `DER_OOG` from sc26; NIC log) | – |
| `e2sar_lb686_1794434906` | 686 | – (destroyed) | – | – | ✔ `lb686/` | – |
| `e2sar_lb686_1794441647` | 686 | – (destroyed) | – | ✔ 2201 `/scratch/xmei` | ✔ `lb686/` | – |
| `e2sar_lb686_1794444385` (--live-rm) | 686 | – (emptied) | ✔ 2203: 1 GiB sample (`_3gbps_1GiB.evio`); the 809 bundle files are gone | ✔ 2203 `/scratch/epsdaq` | ✔ `lb686/` + `deleted_oldest_files.txt` | [--live-rm](2026-11-12-004630UTC-lb686-live-rm.md) |
| `e2sar_lb686_1794459212` (10 TB, 2 receivers) | 686 | – (destroyed) | ✔ 2201: 1 GiB sample (`_20gbps_1GiB.evio`) | ✔ 2201 `/scratch/xmei` and the log dir | ✔ `lb686/`, laptop loss table | [10 TB](2026-11-12-045334UTC-lb686-two-workers-10tb.md) |
| `e2sar_lb686_1794515182` (45 Gbps) | 686 | – (deleted) | – | ✔ log dir | ✔ `lb686/`, laptop copy + loss plot | [rate summary](2026-11-12-rate-summary.md) |
| **`e2sar_lb686_1794515844`** (40.8 Gbps, 10 TB) | 686 | **✔ sc-demo, 10.10 TB, 26.53M events** | ✔ 2201: 1 GiB sample (`_40gbps_1GiB.evio`) | ✔ 2201 `/scratch/xmei`, laptop | ✔ `lb686/`, laptop copy + loss plot | [40 Gbps](2026-11-12-203725UTC-lb686-40gbps-10tb.md) |
| `e2sar_lb686_1794933008` (2026-11-17) | 686 | – | ✔ 2203: 1 GiB sample | – | – | – |
| `e2sar_lb686_1794933463` (2026-11-17) | 686 | – | ✔ 1 GiB sample on **both** nodes (same size, 1,073,705,964 B) | – | – | – |
| Perlmutter sweep `pm_20261027_*` (8 runs) | 640, Perlmutter | – (sc26) | – | – | ✔ `pm/`: per-event CSV (`recv_s, commit_s, latency_ms`), NIC, `nstat` per node; laptop `pm-bench/` | [Perlmutter sweep](2026-10-27-perlmutter-rate-sweep.md) |
| Receiver tuning `sw_*` (68 runs) | – (node to node) | – (sc26) | – | – | ✔ `sweep/` | [tuning](2026-10-29-receiver-tuning.md) |
| E2E bench `xm_n2n_1793327296` | – (node to node) | – | – | – | ✔ `bench/` (`result.json`, `send.csv`, digests, NIC) | [E2E bench](2026-10-19-e2e-bench.md) |
| LUX `e2sar_perf` 2026-10-28/29 (5 runs) | 640/645, LUX | – | – | – | laptop `lux-bench/` | [rate summary](2026-11-12-rate-summary.md) |

## Gone

- **Pool sc26** and everything in it: the LB 675, LB 677 and LB 682 containers, and the October sweep containers. Ranks 4 and 5 are no longer in the DAOS system. Any analysis of those runs has to start from the logs, the monitor lines and the files listed above.
- **The LB 675 and LB 677 reader bundles.** They were moved to ebpf2201 `/scratch/epsdaq` on 2026-11-05 and are no longer there; only their monitor JSON lines are. The LB 675 joined EVIO file is not in the laptop's `~/Downloads` either.
- **The `--live-rm` bundles of `e2sar_lb686_1794444385`** (809 files on ebpf2203): gone; the 1 GiB sample is left.

## Unexplained (to check)

- `e2sar_lb686_1794933008` and `e2sar_lb686_1794933463`: 1 GiB EVIO samples written on 2026-11-17 16:38–17:24 UTC, with no logs in `~/e2sar_run`, no container in sc-demo and no result page. Probably from a run with the new `scripts/e2sar_workflow.sh` (`LOGDIR` default `e2sar_run/workflow`, which doesn't exist). Who ran it, and where its logs are?
- `e2sar_perlmutter_lb682_1794639960`: `perlmutter/current` points to it. Its receiver started on 2026-11-14 against sc26 after LB 682 was freed, and logged only `DER_OOG`. Nothing to keep but the NIC log; candidate for deletion.

## Analyses these data allow (to do)

### LB 686 40 Gbps run in sc-demo (the only full run left)

- [ ] **Read rate from DAOS at scale:** `e2sar_daos_read --fetch` from TB nodes and both ebpf nodes against 10.10 TB / 26.53M events of ~381 KB; the sc26 test (2026-11-11) only had ~9.5 KB events. Find the readers per node and nodes needed to read at the 40 Gbps write rate. Don't start 128 readers at once without asking.
- [ ] **Event-number gaps vs loss causes:** read the index (`--list`) and match the gaps against the 1 s NIC counters in `nic_ebpf220x.log`, to place the 21% "upstream" losses in time (bursts at fixed minutes, like LB 675/677's :27:50 and :57:50?).
- [ ] **Per-sender share:** the three senders (198.124.49.31–.33) by dataId or source, and how evenly the LB spread events over the two receivers' slots.
- [ ] **Physics content:** EVIO frames over the whole run (time-frame numbers, ROC ids, hit counts per frame) with `evio-interactive-view` / `pyevio` on restored files: is the content continuous across 33 minutes?
- [ ] **Redundancy/recovery test** before deleting: does a `--rd-fac` copy of a slice survive excluding a target? (sc-demo has room for ~2 TB.)
- [ ] Decide whether to keep it: it holds 82% of sc-demo.

### The seven 1 GiB EVIO samples

- [ ] **Hit compression on fresh data:** run the [hit-compression](../concepts/hit-compression.md) prototype (zstd, Parquet, delta-t hit encoding) on the 3, 20 and 40 Gbps samples; the published numbers are from LB 677 only.
- [ ] **Event-size and content distribution** per sample (frames per record, ROCs, hit words) to see how the sender changed between 2026-11-12 and 2026-11-17.
- [ ] Use them as fixed replay inputs (`e2sar_evio_send`) for node-to-node regression tests at known event sizes.

### Monitor JSON lines (13 runs)

- [ ] **One plot across runs** of commit rate, DAOS free space and lag vs time, from the JSON lines (`viz/daos_monitor/serve.py` replays one run; a script could overlay all).
- [ ] **Burst timing across LBs:** check every run for losses at the same minute-of-hour (LB 675, 677 showed :27:50/:57:50) to point at a periodic upstream source.

### Receiver, NIC and reader logs (`~/e2sar_run`)

- [ ] **Loss by cause for every LB run,** not only LB 686: run `scripts/e2sar_loss_timeline.py` on the LB 677 runs 1–2 and the three LB 682 runs, and add them to [the rate summary](2026-11-12-rate-summary.md).
- [ ] **Reader throughput vs bundle size** from the `read.log` of the LB 682 and LB 686 `--live`/`--live-rm` runs (the `--live-rm` reader was the bottleneck at ~1.8 Gbps).
- [ ] **Receiver start-up losses:** the first seconds of every `recv_*.log` (start-of-run losses are the main n2n loss).

### Per-event latency (Perlmutter sweep, `pm/`; E2E bench, `bench/`)

- [ ] **Throughput from latency:** apply [Theoretical throughput from latency measurements](../howto/throughput-from-latency.md) to the per-event CSVs: in-flight events (Little's law), the per-thread service time and the predicted ceiling, compared with the measured rates.
- [ ] **Tail latency vs NIC events:** line up the >10 ms commits (550–830 per run) with the 1 s NIC and `nstat` samples.

### iperf3 nightly (`~/daq/iperf3-nightly/history.csv`)

- [ ] **Network baseline over 5 weeks:** UDP lossless rate, TCP and TCP-max rates and retransmits per night, both directions. Did the path change on any date, and do the bad nights match bad E2SAR runs?
- [ ] Add a nightly `ping` RTT/jitter column to the LB data plane, so every run has a matching RTT.

### Laptop bench folders

- [ ] **LUX vs Perlmutter** at the same event size (1 MiB): losses vs rate from `lux-bench/lux_parsed.json` and `pm-bench/summary.csv` in one chart.

## Related

- [Rates and losses by setting](2026-11-12-rate-summary.md) and [the end-to-end summary](e2e-summary.md).
- [Replay a run in the DAOS monitor and the EVIO live viewer](../howto/replay-run.md): what the monitor lines and bundles can still show.
