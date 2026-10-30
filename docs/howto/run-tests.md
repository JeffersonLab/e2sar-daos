---
type: How-To
title: "Testing e2sar-daos"
description: "The test scenarios (Mac, ebpf testbed, LB, Slurm TB, Aurora), the unit and integration suites, and which environment runs which."
tags: [testing, meson, suites]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/TESTING.md", title: "TESTING.md at 0d85d00" }
---

# Testing e2sar-daos

## Test scenarios

| # | Scenario | DAOS | What it checks | Procedure |
|---|---|---|---|---|
| 1 | **Local Mac** | none; client headers and libraries only, in the Rocky 9 container | It builds and links, and the `unit` suite passes | [§1](../sites/mac-container.md#1-mac-without-daos) |
| 2 | **ebpf2201 ↔ ebpf2203, node-to-node** | pool `sc26`, native `daos-devel` | `unit` and `integration` suites. End to end: `e2sar_ft` → `e2sar_daos_recv` → DAOS → `e2sar_daos_read`, with loopback on one node and across the two nodes, with no LB in the path | [§2](../sites/ebpf-testbed.md#2-ebpf2201--ebpf2203-daos-pool-and-daos-devel), [§4](../sites/ebpf-testbed.md#4-applications-end-to-end-on-ebpf2203) |
| 3 | **ebpf2201 → ESnet EJFAT LB → ebpf2203** | pool `sc26`, native `daos-devel` | The same end-to-end path with `--withcp`, through a reserved LB over the WAN: the sender is added to the allowlist, and the worker registers, gets slots and deregisters. Also a throughput sweep, lossless up to 3 concurrent 2 GiB events (~25.8 Gbps) | [lb-test](lb-test.md) |
| 4 | **Slurm TB nodes** | pool `sc26` and `daos_agent`, but **may have no `daos-devel`** (and no E2SAR) | `unit` and `integration` suites and the end-to-end path, using the podman image, which brings its own DAOS headers and E2SAR | [§3](../sites/slurm-tb.md#3-slurm-tb-nodes-daos-pool-but-no-daos-devel), [containers/slurm-tb](../../containers/slurm-tb/README.md) |
| 5 | **ALCF Aurora** (planned, not run yet) | pool `e2sar-daos` (100 TB), DAOS client on the compute nodes | `unit` and `integration` suites, then the end-to-end path and the n2n bench between Aurora compute nodes over the Slingshot network. Section 5 is the plan and the checks still open | [§5](../sites/aurora-plan.md#5-alcf-aurora-planned) |

Scenario 1 needs only Docker. Scenarios 2 and 3 need SSH access to the ebpf nodes, and scenario 3 also needs an LB instance URI. Scenario 4 needs a Slurm TB allocation and podman. Scenario 5 needs an ALCF project with access to pool `e2sar-daos` and a PBS allocation on Aurora.
For throughput and latency of scenarios 2 and 3 with the EVIO time frames of a file streamed as events instead of whole files, see [e2e-bench](e2e-bench.md) (`scripts/e2sar_e2e_bench.sh`).
Scenarios 2 and 3 share the same nodes with other users. Use your own port, container and `/scratch` directories, and clean up after every run.

## Test suites

There are two test suites:

| Suite | Test | Needs |
|---|---|---|
| `unit` | `DAOSNativeTests`: checks that `<daos.h>` compiles and links, and the DAOS major version is 2.<br>`EventLayoutTests`: checks the key names and 1 MiB chunking that `e2sar_daos_recv`/`e2sar_daos_read` use.<br>`RunCatalogTests`: checks the per-run object ids (FNV-1a hash of the prefix) and the run catalog's keys and records.<br>`LayoutTests`: checks the run layout of [daos-layout](../concepts/daos-layout.md): object ids, index keys, batch tables and catalog records.<br>`LatencyHistogramTests`: checks the latency percentiles in `e2sar_daos_recv`'s timing summary | DAOS headers and libraries, no DAOS server |
| `integration` | `DAOSKVTest`: writes 5 random 0.5 MiB values into a DAOS KV object, reads them back and compares SHA-256 digests | A reachable DAOS pool and container, plus `openssl-devel` at build time |

`DAOSKVTest` reads the pool and container from `DAOS_POOL` and `DAOS_CONT` (defaults `sc26` and `kv-test`).
`meson test` sets both from the meson options `daos_pool` and `daos_cont`.

What each environment can check:

| Environment | Build | `unit` | `integration` |
|---|---|---|---|
| [Mac (no DAOS)](../sites/mac-container.md#1-mac-without-daos) | in the Docker container | yes | no |
| [ebpf2201 / ebpf2203](../sites/ebpf-testbed.md#2-ebpf2201--ebpf2203-daos-pool-and-daos-devel) | natively | yes | yes |
| [Slurm TB nodes](../sites/slurm-tb.md#3-slurm-tb-nodes-daos-pool-but-no-daos-devel) | in a podman container (or on ebpf220x) | yes | yes |
| [ALCF Aurora](../sites/aurora-plan.md#5-alcf-aurora-planned) | natively on a compute node (planned) | to do | to do |

The applications (`e2sar_daos_recv`, `e2sar_daos_read`) are tested end to end by hand. See [section 4](../sites/ebpf-testbed.md#4-applications-end-to-end-on-ebpf2203).

---
