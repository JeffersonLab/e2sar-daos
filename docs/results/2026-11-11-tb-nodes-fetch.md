---
type: Result
title: "Reading sc26 from 16 Slurm TB nodes with e2sar_daos_read --fetch (2026-11-11)"
description: "e2sar_daos_read --fetch on the Slurm TB nodes, reading LB 677 run 1 (161 GB, ~9.5 KB events) from sc26. One reader process is CPU-bound on one thread at 1.2–1.3 Gbps on a tb16 node, 1.6 Gbps on tb18c07 and 1.8 Gbps on tblogin1902; 16 nodes with one reader each read 14.8 Gbps together (sum of per-node rates), half of them waiting on DAOS. A run can't be split between readers. Right after a 128-reader test (8 per node) every data read in sc26 timed out on engine rank 4 (daosfs2302), still at 21:45 UTC. Lists the bugs, failures and proposals."
tags: [slurm-tb, sc26, e2sar_daos_read, read, bug, proposal]
sources:
  - { id: reader, resource: "../reference/e2sar_daos_read.md", title: "e2sar_daos_read" }
  - { id: site, resource: "../sites/slurm-tb.md", title: "Slurm TB nodes" }
  - { id: run1, resource: "2026-11-05-194730UTC-lb677-run1-evio.md", title: "LB 677 run 1 (the run read here)" }
---

# Reading sc26 from 16 Slurm TB nodes with e2sar_daos_read --fetch (2026-11-11)

How fast can the Slurm TB nodes read a stored run back out of DAOS? This is the read side of a pipeline where ebpf2201/2203 receive from the LB and write to DAOS, and TB nodes read from DAOS.

**Highest rates**

| | Gbps | Where |
|---|---|---|
| One reader process | **1.80** | tblogin1902 (2× Xeon Gold 6148), 30 s |
| One reader process on a compute node | **1.61** | tb18c07, during the 16-node run |
| One reader process on a tb16 node, alone | 1.20–1.30 | tb16c01, three 60 s runs |
| 16 nodes × 1 reader, together | **14.8** (sum of per-node rates); 10.3 over the whole 153 s window | 15 tb16 nodes + tb18c07 |

The reader is the limit, not the network or DAOS: one process uses ~0.85 of a core and the TB nodes' InfiniBand link is 56 Gbps.

## Setup

| | |
|---|---|
| Reader app | `e2sar_daos_read --pool sc26 --cont <run> --fetch` ([reference](../reference/e2sar_daos_read.md)): reads every event of the run and prints one `<sha256>  <name>  <bytes> bytes` line per event. Opens the pool read-only, writes nothing. Defaults `--inflight 32 --read-size 16M`. Built on ebpf2201 from `main` at 6f43010 (`~/daq/e2sar-daos/build`), run natively with `LD_LIBRARY_PATH=$HOME/opt/e2sar-0.4.0rc1/usr/local/lib` (Boost 1.89, see [bug 5](#bugs-and-failures)) |
| Run read | `e2sar_lb677_1793905825` ([LB 677 run 1](2026-11-05-194730UTC-lb677-run1-evio.md)): 17,019,790 events, 161.3 GB, 9,477 B mean, one dataId, 2 writer slots, 1,116 seconds with data |
| Pool | `sc26`: 32 targets on two engines, ranks 4 and 5, both on daosfs2302 (rank 4 `daosfs2302-ib3` 172.19.27.21, rank 5 `daosfs2302-ib1` 172.19.27.20). Provider `ofi+verbs;ofi_rxm` |
| Nodes | tb16c01–03, tb16c05–16 (Broadwell, 36 cores, 60 GB, ConnectX-3 FDR 56 Gbps) and tb18c07 (Skylake, 40 cores, ConnectX-5 at FDR). tb16c04 left out ([failure 4](#bugs-and-failures)); tb18c06 was busy. Partition `cores` |
| Script | `~/daosbench/fetch1.sh CONT SECONDS` on the testbed: waits for a common start time (`START_AT`), runs the reader under `timeout -s INT`, counts the event lines and their bytes. Rate = bytes / the task's own wall time, start-up included |
| Launch | `srun -p cores -w <16 nodes> -N 16 --ntasks-per-node=1 -c 32 ~/daosbench/fetch1.sh e2sar_lb677_1793905825 90`. All readers read the same run from the start: the reader has no way to split a run ([proposal 1](#proposals)) |

`--fetch` prints the events of 16 seconds of data at once (one group, ~244k events, ~2.3 GB here), so event counts move in steps of a group and a 90 s rate is good to about ±0.2 Gbps. After SIGINT the reader finishes its current group, so runs last longer than 90 s.

## One reader on one node

| Node | Slurm job | Events | Wall time | Gbps | kevents/s | CPU |
|---|---|---|---|---|---|---|
| tblogin1902 (login node) | — | 732,509 (6.76 GB) | 30 s | 1.80 | 24.4 | — |
| tb16c01 | 112 | 1,220,786 | 71.4 s | ≈1.30 | 17.1 | 0.85 cores |
| tb16c01 | 113 | 976,639 | 61.5 s | ≈1.20 | 15.9 | 0.84 cores |
| tb16c01, `--inflight 256` | 114 | 1,220,786 | 72.5 s | ≈1.28 | 16.8 | 0.85 cores |

The tb16c01 bytes are events × the 9,477 B mean (the script summed the wrong field in these runs). `--inflight 256` changes nothing: the reader's one thread (SHA-256 per event, one printed line per event, group by group) is busy all the time.

## 16 nodes, one reader each (job 115, 20:39–20:42 UTC)

| Node | Events | GB | Wall time | Gbps | CPU |
|---|---|---|---|---|---|
| tb18c07 | 2,197,339 | 20.20 | 100.2 s | 1.61 | 1.01 |
| tb16c01 | 1,709,059 | 15.66 | 100.3 s | 1.25 | 0.85 |
| tb16c02 | 1,709,059 | 15.66 | 102.2 s | 1.23 | 0.83 |
| tb16c06 | 1,709,059 | 15.66 | 101.5 s | 1.23 | 0.84 |
| tb16c12 | 1,709,059 | 15.66 | 103.3 s | 1.21 | 0.83 |
| tb16c05 | 1,464,920 | 13.43 | 92.2 s | 1.17 | 0.82 |
| tb16c09 | 1,464,920 | 13.43 | 92.7 s | 1.16 | 0.82 |
| tb16c11 | 1,464,920 | 13.43 | 93.4 s | 1.15 | 0.82 |
| tb16c16 | 1,464,920 | 13.43 | 93.9 s | 1.14 | 0.82 |
| tb16c03 | 1,464,920 | 13.43 | 152.1 s | 0.71 | 0.50 |
| tb16c14 | 1,464,920 | 13.43 | 153.3 s | 0.70 | 0.51 |
| tb16c08 | 976,639 | 9.06 | 120.6 s | 0.60 | 0.43 |
| tb16c13 | 976,639 | 9.06 | 120.4 s | 0.60 | 0.43 |
| tb16c15 | 732,509 | 6.76 | 96.3 s | 0.56 | 0.39 |
| tb16c07 | 488,355 | 4.48 | 141.3 s | 0.25 | 0.20 |
| tb16c10 | 488,355 | 4.48 | 152.7 s | 0.23 | 0.19 |
| **Total** | 23.4M | 197.3 | 153.3 s window | **14.8** (sum) / 10.3 (window) | |

Two groups: nine readers ran at the single-reader rate with their thread busy (~0.82 cores), seven read 0.2–0.7 Gbps with their thread mostly idle, waiting on DAOS ([failure 7](#bugs-and-failures)). Each tb16 node at full speed carries ~1.2 Gbps of its 56 Gbps link.

## 16 nodes, eight readers each (job 116, 20:42–20:48 UTC): no result

`--ntasks-per-node=8 -c 4`, 128 readers of the same run, each with up to 32 reads of 16 MiB in flight. No reader reported within the job's 6 min limit and Slurm cancelled the step (`TIMEOUT`). From the next test on, data reads in sc26 failed ([failure 1](#bugs-and-failures)).

## Bugs and failures

1. **Failure: every data read in sc26 times out on engine rank 4.** First seen in job 117 (20:49–20:51 UTC): three readers (tb16c01, c07, c10) read 0 events in 120 s. On tblogin1902 at 20:57 and on ebpf2201 at 20:58 UTC, reads of a small run (`e2sar_perlmutter_lb682_1794396455`) failed with
   `dc_rw_cb() ... RPC ... to 4/8, flags 0/2020 ... failed, DMA ... DER_TIMEDOUT(-1011)`
   Still failing at 21:45 UTC. `daos pool query sc26` (state Ready, 32 targets, 0 disabled, rebuild idle) and index reads (`--list`) work. It started right after job 116; that it caused it is likely but not shown. Needs a DAOS admin: rank 4 is daosfs2302 (`daosfs2302-ib3`, 172.19.27.21), target 8.

   **Writes too, at 22:02 UTC.** `e2sar_daos_recv` on ebpf2201 took LB 686 traffic into a new container and committed 20,893 events in 10 s; then every write to rank 4, target 8 timed out (`DER_TIMEDOUT`), the receiver stopped receiving and needed `SIGKILL` after Ctrl-C, and `--list` on the new container hung too. A live pipeline run at 22:08 UTC stalled the same way after 11,889 events. The pool still reported Ready with 0 disabled targets. Copies (`--rd-fac`, see [Redundancy](../concepts/daos-layout.md#redundancy)) wouldn't have kept writes going, but would make excluding the target lose no data.
2. **Bug: `--fetch` doesn't stop on SIGINT/SIGTERM while reads hang.** The reader checks for a signal only between 16-second groups ([e2sar_daos_read.cpp](../../bin/e2sar_daos_read.cpp), `forEachGroup`). With failure 1, a reader on tblogin1902 still ran 3.5 min after its signal and needed SIGKILL; job 116's readers didn't stop either. The same loop serves `--offline` and `--list`.
3. **Bug: `--fetch --event N` reads the index of every second.** On this run, `--fetch --event 5` printed the event within seconds, then went on through the rest of the run's index and didn't finish in 60 s. `--dataid` and `--offline`/`--list` with them do the same.
4. **Failure: tb16c04 can't reach DAOS.** Every `daos` call fails with `GetAttachInfo unsuccessful: 2` / `DER_MISC(-1025)`. Its `daos_agent` has been running since 2026-06-04, but DAOS 2.6.5 was installed on 2026-10-16; the other nodes restarted their agent after that. Needs `systemctl restart daos_agent` by an admin; until then `--exclude=tb16c04`.
5. **Bug: the build doesn't find Boost 1.89 at run time.** `ldd build/bin/e2sar_daos_read` shows `libboost_program_options.so.1.89.0 => not found`, on ebpf2201 too, in any shell that hasn't sourced `~/.bashrc` (as under `srun`). Works with `LD_LIBRARY_PATH=$HOME/opt/e2sar-0.4.0rc1/usr/local/lib`.
6. **Bug (docs): [slurm-tb](../sites/slurm-tb.md) said the TB nodes have no `daos-devel`.** They have `daos-devel` 2.6.5 (checked on tb14c01, tb16c01, tb18c07 and the login node). Fixed with this result.
7. **Failure, open: half the readers waited on DAOS in the 16-node run.** Seven of 16 readers read 0.2–0.7 Gbps with an idle thread while the others ran CPU-bound. Not tied to a node type (all but one are tb16). All 16 read the same seconds at about the same time, on the same targets of one server (daosfs2302); whether that or something on the slow nodes is the cause is open: the re-test (job 117) ran into failure 1.

## Proposals

1. **`--part i/N` for `--fetch`, `--offline` and `--live`.** Reader `i` of `N` takes every Nth second (or group of seconds), so `srun -n N` with `--part $SLURM_PROCID/$SLURM_NTASKS` splits one run with no overlap. Today a run can be split only by dataId, and the runs in sc26 have one.
2. **A read-only benchmark mode**, e.g. `--fetch --no-hash`: count bytes, print one summary line per group instead of a flushed line per event, and hash on worker threads when hashing is on. Then a test measures DAOS and the network instead of one thread's SHA-256.
3. **Overlap the groups**: read the next group's index while the current group's data is in flight, instead of one group at a time.
4. **Stop promptly on a signal** (bug 2): check the stop flag in the read loop and abort or time out in-flight reads.
5. **Stop `--event` / `--dataid` early** (bug 3), or find the event's second from the index instead of scanning all of them.
6. **Set an rpath for Boost** in `meson.build` (bug 5), or document the `LD_LIBRARY_PATH` in [build](../howto/build.md).
7. **Limit the load of a many-reader test**: readers × `--inflight` × `--read-size` was up to 128 × 32 × 16 MiB = 64 GiB requested from two engines at once. Ramp up the number of readers, and ask the DAOS admins before going past ~16.
8. **A hardware baseline with IOR** (`-a DFS`): Open MPI 4.1.1 (`openmpi-devel`) and `daos-devel` are on the nodes; build IOR from its release tarball (no autotools on the nodes). That gives the per-node DAOS read rate this reader can't reach.
9. **Admin requests**: check engine rank 4 on daosfs2302 (failure 1); restart `daos_agent` on tb16c04 (failure 4).
