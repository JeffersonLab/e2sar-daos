---
type: Site
title: "ALCF Aurora (planned)"
description: "Why Aurora needs its own procedure (PBS, SLES, Slingshot, DAOS on compute nodes only), the test phases, and the open questions. Not yet run."
tags: [aurora, alcf, pbs]
status: draft
stale_after: 2027-01-31T00:00:00Z
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/TESTING.md", title: "TESTING.md at 0d85d00" }
---

# 5. ALCF Aurora (planned)

**Status: this is a plan, and nothing has run on Aurora yet.** Lines marked *(check)* are assumptions about Aurora. Confirm each one on the machine, then edit this section to match what you find.

The project has a 100 TB DAOS pool on Aurora, **`e2sar-daos`**.
This section covers why Aurora needs a different procedure and what to test. For the commands, in order, with PBS job scripts, see [aurora-workflow](aurora-workflow.md).
Aurora is unlike the JLab hosts in four ways that shape the plan:

| | JLab (ebpf220x, TB) | Aurora | What it means for testing |
|---|---|---|---|
| Scheduler | none (ebpf220x), Slurm (TB) | **PBS Pro** (`qsub`, `qstat`, `$PBS_NODEFILE`) | Every DAOS test runs inside a PBS job with `daos_user` in `-l filesystems=`, which is how ALCF documents requesting DAOS. Run `qstat -Bf` and look at `resources_available.valid_filesystems` for the valid names. The `srun`/`sbatch` commands of section 3 don't apply |
| Where DAOS is reachable | on every host | **compute nodes only** *(check)*, not the login nodes (UANs) | Build on a login node if the DAOS headers are there. Run `meson test` and the applications on a compute node |
| OS and packages | AlmaLinux/Rocky 9 (RHEL family), `dnf`, `daos-devel` RPM | **SLES 15 SP7** on compute nodes and UANs (rolled out 2026-09-28, [ALCF system updates](https://docs.alcf.anl.gov/aurora/system-updates/)), no root, environment modules (`module use /soft/modulefiles; module load daos`, or `daos/base`) *(check)* | E2SAR has no SLES package, so build E2SAR from source. Packages built for EL9 (the E2SAR RPM, the dev container's prebuilt E2SAR) won't run on SLES. The DAOS headers may not be in `/usr/include`, which the top-level `meson.build` hard-codes |
| Shell | bash | bash (default login shell) | The exports from section 2 go in `~/.bashrc`, which `scripts/e2sar_e2e_bench.sh` sources |
| Network | 100 GbE, MTU 9000, DAOS over TCP/verbs | HPE Slingshot 11, several `hsn*` NICs per node, DAOS over `ofi+cxi` *(check)* | UDP between compute nodes goes over the `hsn*` IPs. A container would need the host's CXI libfabric, so the podman route of section 3 doesn't carry over. Build natively instead |

Inbound traffic from outside ALCF to the compute nodes is blocked *(check)*, so the ESnet LB path (scenario 3) can't reach a receiver on Aurora.
On Aurora, both the sender and the receiver are Aurora compute nodes, and the bench runs node to node (n2n), with no LB in the path.
Whether data from JLab or the LB can get into Aurora at all is a separate question for ALCF (see [Open questions](#open-questions)).

## Phase 0: find out what's there (one interactive job)

```bash
# login node: interactive PBS job with DAOS access
qstat -Bf | grep valid_filesystems               # confirm the name daos_user
qsub -I -A <project> -q debug -l select=1 -l walltime=00:30:00 \
     -l filesystems=home:flare:daos_user

# compute node
module use /soft/modulefiles && module load daos
daos version                                   # major must be 2 (DAOSNativeTests checks this); note the minor
daos pool query e2sar-daos                     # reachable as your user? free space, number of targets
daos pool get-prop e2sar-daos                  # default redundancy (rd_fac), which limits the allowed object classes
daos cont list e2sar-daos                      # who else uses the pool
ls /usr/include/daos.h /usr/lib64/libdaos.so   # headers and libraries; if not here: module show daos
grep -iE 'provider|runtime_dir' /etc/daos/daos_agent.yml
ip -br addr show | grep hsn                    # hsn IPs
cat /sys/class/net/hsn0/device/numa_node       # which NUMA node each hsn NIC is on
ip link show hsn0 | grep -o 'mtu [0-9]*'       # the MTU sets the largest E2SAR datagram
gcc --version; module avail gcc boost cmake    # toolchain for E2SAR
```

Note the answers in this section. They decide the next phases.

## Phase 1: build

1. **E2SAR.** Build it from source on a login node into `$HOME/opt/e2sar` (or a project directory on Flare). The no-root route in [install-e2sar](../howto/install-e2sar.md) is written for EL9, so use its source build, not the RPM. Building E2SAR's dependencies (gRPC, protobuf, Boost) may be the hardest part on SLES 15 SP7. Check first whether `module avail` already has a usable GCC, CMake, Boost or gRPC. If the bundled dependency build fails, try Spack.
2. **e2sar-daos.** Set the same exports as in section 2 (`PKG_CONFIG_PATH`, `PKG_CONFIG_SYSROOT_DIR`, `BOOST_ROOT`, `LD_LIBRARY_PATH`), then run `meson setup build && meson compile -C build`.
   If the DAOS headers aren't in `/usr/include` and `/usr/lib64`, `meson setup` stops with `DAOS libraries or headers not found`.
   The fix is a meson option for the DAOS prefix (e.g. `-Ddaos_prefix=...`) instead of the hard-coded paths. Make that code change only if Phase 0 shows it's needed.
3. Build on a login node only if it has the same DAOS headers as the compute nodes. Otherwise build in a short compute job.

## Phase 2: test suites on a compute node

```bash
daos cont create e2sar-daos kv-test            # once
meson configure build -Ddaos_pool=e2sar-daos -Ddaos_cont=kv-test
meson test -C build --suite unit -v            # Ok: 4
meson test -C build --suite integration -v     # Ok: 1, 5 matching put/get SHA-256 pairs
```

Give the pool name explicitly, because the meson default is `sc26`, the JLab pool.
Once this works interactively, the same commands can run as a batch job (`qsub job.sh`, with the same `#PBS -l` lines as the `qsub -I` above, plus `#!/bin/bash` and `module use /soft/modulefiles && module load daos` at the top).

## Phase 3: applications end to end

Follow the same procedure as [section 4](ebpf-testbed.md#4-applications-end-to-end-on-ebpf2203), inside a PBS job, with `--pool e2sar-daos`:

1. **One node, loopback.** Run `e2sar_daos_recv --ip 127.0.0.1` and `e2sar_ft -s` on the same compute node, then `e2sar_daos_read --fetch`, and compare digests.
2. **Two nodes over Slingshot.** In a 2-node job (`-l select=2`), take the two hostnames from `$PBS_NODEFILE` and run the receiver on the second node (`mpiexec -n 1 --hosts <node2> ...`, or `ssh <node2>` within the job). Use node2's `hsn0` IP in `EJFAT_URI` and `--ip`.
   First check that UDP gets through between the two `hsn` IPs *(check)*, e.g. with `iperf3 -u` if it's available, or with a short `e2sar_ft` run.

## Phase 4: throughput and latency (n2n bench)

[`scripts/e2sar_e2e_bench.sh`](../../scripts/e2sar_e2e_bench.sh) `-m n2n` should work unchanged when the Aurora values are set in the environment, because its defaults are for the ebpf testbed:

```bash
export POOL=e2sar-daos CONT=e2sar-bench-$USER
export SEND_IP=<node1 hsn0 ip> RECV_IP=<node2 hsn0 ip> NIC=hsn0
export RUN=$HOME/e2sar_run        # rendezvous files; $HOME must be shared by both nodes
```
`NIC` is only used for the drop counters. Slingshot NICs may not report `rx_discards_phy`, so check what `ethtool -S hsn0` shows and adapt the script if needed.

Sweep one variable at a time, in this order, and compare with [e2e-bench](../howto/e2e-bench.md):

| # | Vary | Values | Question |
|---|---|---|---|
| 1 | sender rate `-r` | 10, 20, 40, 60, 80 Gbps | highest lossless rate for one receiver on one NIC |
| 2 | dequeue threads `-D` | 4, 16, 32, 64 | at what rate the DAOS side stops keeping up. At JLab, 16 threads reached 40 Gbps |
| 3 | object class (`--oclass` on the receiver) | container default, `SX`, `RP_2G1`, EC classes the pool's `rd_fac` allows | what redundancy costs on a large pool |
| 4 | – | – | (dropped: the dfuse and pil4dfs sinks were removed) |
| 5 | NUMA and NICs | `--numa` and `--cores` matching the `hsn` NIC; one receiver per NIC | the per-node ceiling with several NICs |
| 6 | receiver nodes | 1, 2, 4, 8 receivers, one sender each | whether the total scales with nodes, i.e. whether the pool is the limit |

Run each point at least twice, as at JLab. Load from other projects on the pool can change results between jobs, so record the time and the PBS job ID with each result.

## Clean up (after every job)

The pool is shared and has a quota, so remove the test data before the job ends:
```bash
daos cont destroy e2sar-daos e2sar-test
daos cont destroy e2sar-daos e2sar-bench-$USER
daos cont list e2sar-daos                      # only kv-test and containers in use should remain
rm -rf /tmp/$USER/e2sar_*                      # node-local /tmp is wiped at the end of the job anyway
```
Don't destroy `kv-test`: the integration test uses it.

## Open questions

- Which DAOS version does Aurora run? Does the minor version matter for `libe2sar-daos`? (JLab runs 2.6.5.)
- Are the DAOS headers on the login nodes, or only on the compute nodes? Is a `daos_prefix` meson option needed?
- Does E2SAR's dependency build work on SLES 15 SP7 with the system GCC, or does it need a newer GCC module or Spack?
- Does UDP between compute nodes over `hsn*` hold up at E2SAR's rates and datagram sizes? What is the MTU?
- Can data from outside ALCF (JLab, or the ESnet LB) reach a compute node at all? If not, is there a gateway or data-transfer node where `e2sar_daos_recv` can run and still reach DAOS? Ask ALCF support. The answer decides whether the real JLab → Aurora path can be tested.
- Which object classes and `rd_fac` does pool `e2sar-daos` allow? What is the quota per user or per project?
