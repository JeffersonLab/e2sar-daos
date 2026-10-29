# e2sar-daos scripts

Bash scripts that drive the [applications](../bin/README.md) for tests on the JLab ebpf nodes. Each one prints its usage when run without arguments, and the comment at its top lists its settings.

| Script | Where it runs | What it does |
|---|---|---|
| [`e2sar_e2e_bench.sh`](e2sar_e2e_bench.sh) | Receiver and sender node, one role each | One end-to-end run: `e2sar_evio_send` → [LB] → `e2sar_daos_recv` → DAOS. Throughput, latency per stage, and a digest check of every event. See [docs/E2E_BENCHMARK.md](../docs/E2E_BENCHMARK.md) |
| [`e2sar_e2e_sweep.sh`](e2sar_e2e_sweep.sh) | A machine that can SSH to both nodes | A rate sweep: runs `e2sar_e2e_bench.sh` on both nodes for each rate and repetition. See [docs/E2E_BENCHMARK.md section 7](../docs/E2E_BENCHMARK.md#7-node-to-node-rate-sweep-2050-gbps-2026-10-20) |
| [`e2sar_site2ebpf_recv.sh`](e2sar_site2ebpf_recv.sh) | ebpf2201 and ebpf2203 | Runs one node as a receiver for a remote sender (Perlmutter, or another site; `SITE` only picks the log directory): pre-flight checks, shared run directory, `e2sar_daos_recv` (with `-v`) into a new container `<CONT_LABEL>_<UTC epoch s>` per run, drop checks after Ctrl-C, then `verify N`, `runs` (the runs in DAOS, from the run catalog) and `clean [RUNID]` (destroy one run's container, or every `<CONT_LABEL>_<epoch s>` container). See [docs/PERLMUTTER_GUIDE.md](../docs/PERLMUTTER_GUIDE.md#the-receiver-script) |

## Before running them

- **Build first.** The scripts run the apps from the build next to them, `../build/bin/` (`meson compile -C build`). Set `E2SAR_DAOS_BUILD` to use another build. Run the copy of a script in the checkout whose build you want, e.g. in a separate worktree.
- **Environment.** They read `~/.bashrc` for `E2SAR_BIN` (the E2SAR tools, such as `lbadm`) and, through the LB, the instance URI (`EJFAT_URI`). `e2sar_site2ebpf_recv.sh` takes the URI as `EJFAT_URI` (passed in or exported) and its listen address from `MY_IP`. See [docs/ESNET_LB_TESTING.md step 1](../docs/ESNET_LB_TESTING.md#1-environment-bashrc-and-the-lb-instance-once).
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

The analysis step of the bench, [`bin/e2sar_e2e_analyze.py`](../bin/e2sar_e2e_analyze.py), stays in `bin/` next to the apps, and `e2sar_e2e_bench.sh` calls it from there.
