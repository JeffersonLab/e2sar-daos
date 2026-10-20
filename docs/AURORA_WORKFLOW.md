# Testing workflow on ALCF Aurora

The commands for one test cycle on Aurora, in order: one-time setup, build, then one PBS job per test stage.
For why Aurora needs a different procedure, the benchmark sweeps and the open questions, see [TESTING.md, section 5](TESTING.md#5-alcf-aurora-planned).

**Status: not yet run on Aurora.** Steps marked *(check)* rest on assumptions. When a step behaves differently, fix it here and update section 5.

| | |
|---|---|
| System | Aurora, SLES 15 SP7, PBS Pro, bash |
| DAOS pool | `e2sar-daos` (100 TB, shared by the project) |
| Containers | `kv-test` (integration test, keep), `e2sar-test` (end to end), `e2sar-bench-$USER` (bench). Destroy the last two after every job |
| Where things run | build: login node (UAN). Everything that touches DAOS: compute nodes |

```
 once                       every change                        every job
 ──────────────────────     ───────────────────────────────     ──────────────────────────────────────
 0. ~/.bashrc, E2SAR   ──▶  1. git pull, meson compile     ──▶  2. test suites    (1 node)
    kv-test container                                           3. end to end      (1 node, then 2)
                                                                4. bench           (2+ nodes)
                                                                5. record, clean up
```

Go on to the next stage only when the current one passes.

---

## 0. One-time setup

### Environment (`~/.bashrc`)

Every shell, PBS job and `bin/e2sar_e2e_bench.sh` reads this:

```bash
# e2sar-daos on Aurora
module use /soft/modulefiles 2>/dev/null && module load daos 2>/dev/null   # (check) the module may be daos/base
E2SAR_ROOT=$HOME/opt/e2sar
export E2SAR_BIN=$E2SAR_ROOT/usr/local/bin
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH
export PATH=$HOME/.local/bin:$PATH                # meson, ninja
export DAOS_POOL=e2sar-daos
export PROJECT=<your ALCF project>
```

If E2SAR is installed straight into a prefix instead of a staging root, drop `PKG_CONFIG_SYSROOT_DIR`, as in the [README](../README.md#point-pkg-config-and-meson-to-e2sar).

### E2SAR and the repo

```bash
# login node
python3 -m pip install --user meson ninja
# E2SAR: build from source into $E2SAR_ROOT (see E2SAR_INSTALL.md; the EL9 RPM doesn't run on SLES)
pkg-config --modversion e2sar                     # sanity check

git clone git@github.com:JeffersonLab/e2sar-daos.git ~/daq/e2sar-daos
```

### Integration test container (in the first compute job)

```bash
daos cont create e2sar-daos kv-test
```

---

## 1. Build (login node, after every change)

```bash
cd ~/daq/e2sar-daos && git pull
meson setup build -Ddaos_pool=e2sar-daos -Ddaos_cont=kv-test   # first time; later only meson compile
meson compile -C build
```

`meson setup` must print E2SAR's `Boost version`, and must not print `skipping DAOSKVTest` or `skipping e2sar_daos_read`.
If it fails with `DAOS libraries or headers not found`, the login node has no DAOS headers *(check)*. Then run the build in a short compute job.

---

## 2. Test suites (1 node)

Interactive, the first time:
```bash
qstat -Bf | grep valid_filesystems                # check the name daos_user
qsub -I -A $PROJECT -q debug -l select=1 -l walltime=00:30:00 -l filesystems=home:flare:daos_user
cd ~/daq/e2sar-daos
meson test -C build --suite unit -v               # Ok: 3
meson test -C build --suite integration -v        # Ok: 1
```

After that, as a batch job, `~/daq/jobs/suites.pbs`:
```bash
#!/bin/bash
#PBS -A <project>
#PBS -q debug
#PBS -l select=1
#PBS -l walltime=00:15:00
#PBS -l filesystems=home:flare:daos_user
#PBS -j oe
source ~/.bashrc
cd ~/daq/e2sar-daos
meson test -C build --suite unit -v && meson test -C build --suite integration -v
```
```bash
qsub ~/daq/jobs/suites.pbs; qstat -u $USER
```

**Pass:** `Ok: 3` and `Ok: 1`, with 5 matching `put`/`get` SHA-256 pairs in the log.

---

## 3. End to end (1 node, then 2)

### 3a. One node, loopback

In an interactive 1-node job, follow [TESTING.md, section 4](TESTING.md#4-applications-end-to-end-on-ebpf2203) with these changes:
- Add `--pool e2sar-daos` to both `e2sar_daos_recv` and `e2sar_daos_read`. Both default to `sc26`.
- Run the receiver in the background in the same shell (`... &> /tmp/$USER/recv.log &`), or in a second `ssh` session to the same compute node.
- Source files: copy a few EVIO files to `/tmp/$USER/e2sar_src` on the node, plus the random file.

**Pass:** the `diff` of the digests prints `PASS`, `Events failed: 0`, `Lost in reassembly: 0`.

### 3b. Two nodes over Slingshot

```bash
qsub -I -A $PROJECT -q debug -l select=2 -l walltime=00:30:00 -l filesystems=home:flare:daos_user
mapfile -t NODES < <(sort -u $PBS_NODEFILE)
SEND_NODE=${NODES[0]} RECV_NODE=${NODES[1]}
RECV_IP=$(ssh $RECV_NODE "ip -4 -br addr show hsn0 | awk '{print \$3}' | cut -d/ -f1")   # (check) hsn0 has an IPv4 address
SEND_IP=$(ip -4 -br addr show hsn0 | awk '{print $3}' | cut -d/ -f1)
export EJFAT_URI="ejfat://useless@127.0.0.1:18347/lb/1?data=$RECV_IP&sync=$RECV_IP:19020"
export PREFIX=test_$(date +%s)

# receiver on the second node
ssh $RECV_NODE "source ~/.bashrc; cd ~/daq/e2sar-daos; EJFAT_URI='$EJFAT_URI' \
    ./build/bin/e2sar_daos_recv --ip $RECV_IP --port 19522 --pool e2sar-daos --cont e2sar-test --prefix $PREFIX" \
    &> ~/daq/recv_$PREFIX.log &

# sender on this node
$E2SAR_BIN/e2sar_ft -s --ip $SEND_IP --rate 5 /tmp/$USER/e2sar_src/*
```
When the receiver log shows every file committed, stop it (`ssh $RECV_NODE pkill -INT e2sar_daos_recv`), then verify with `e2sar_daos_read --fetch` as in 3a.
If nothing arrives, check that UDP 19522 gets from `SEND_IP` to `RECV_IP` *(check)* before you suspect E2SAR.

---

## 4. Bench (2 or more nodes)

`bin/e2sar_e2e_bench.sh -m n2n`, with the Aurora values in the environment. The two roles meet through `$RUN` in the shared `$HOME`.
`~/daq/jobs/bench.pbs`:
```bash
#!/bin/bash
#PBS -A <project>
#PBS -q debug
#PBS -l select=2
#PBS -l walltime=00:30:00
#PBS -l filesystems=home:flare:daos_user
#PBS -j oe
source ~/.bashrc
cd ~/daq/e2sar-daos
mapfile -t NODES < <(sort -u $PBS_NODEFILE)
hsn_ip() { ssh $1 "ip -4 -br addr show hsn0 | awk '{print \$3}' | cut -d/ -f1"; }
export POOL=e2sar-daos CONT=e2sar-bench-$USER NIC=hsn0 RUN=$HOME/e2sar_run
export SEND_IP=$(hsn_ip ${NODES[0]}) RECV_IP=$(hsn_ip ${NODES[1]})
TAG=aurora_${PBS_JOBID%%.*}_${RATE:-20}g
ENV="POOL=$POOL CONT=$CONT NIC=$NIC RUN=$RUN SEND_IP=$SEND_IP RECV_IP=$RECV_IP"

ssh ${NODES[1]} "cd $PWD && $ENV bin/e2sar_e2e_bench.sh recv $TAG -m n2n -T 4 -D ${DEQ:-16}" &
ssh ${NODES[0]} "cd $PWD && $ENV bin/e2sar_e2e_bench.sh send $TAG -m n2n -P 4 -r ${RATE:-20}"
wait
```
```bash
qsub -v RATE=20,DEQ=16 ~/daq/jobs/bench.pbs        # one point of a sweep
```
Sweep one variable per job series, in the order of the table in [TESTING.md, Phase 4](TESTING.md#phase-4-throughput-and-latency-n2n-bench). Run each point twice.
`ethtool -S hsn0` may not have the counters the script reads for drops *(check)*. If so, the bench still checks every digest, but the drop column will be empty.

---

## 5. Record and clean up (every job)

**Record.** The bench appends one line per run to `$RUN/bench/results.jsonl`. For stages 2 and 3, note the result by hand. With every result, keep:
- the date, the PBS job ID and the node names,
- `git rev-parse --short HEAD`, the E2SAR version and `daos version`,
- `daos pool query e2sar-daos` free space, because the pool is shared.

**Clean up.** Do this before the job's walltime runs out:
```bash
daos cont destroy e2sar-daos e2sar-test           # if stage 3 ran
daos cont destroy e2sar-daos e2sar-bench-$USER    # if the bench didn't already
daos cont list e2sar-daos                         # only kv-test and containers in use should remain
```
Don't destroy `kv-test`. Node-local `/tmp` is wiped when the job ends, but files in `$HOME` (logs, `$RUN`) are not, so delete old ones from time to time.
