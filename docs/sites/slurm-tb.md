---
type: Site
title: "Slurm TB nodes"
description: "The JLab Slurm TB nodes: their network and DAOS client, build and test with podman (image storage, the daos_agent socket, SELinux, InfiniBand, batch jobs), and running the ebpf2201 build natively, e.g. e2sar_daos_read."
tags: [slurm-tb, podman, sc26, network]
stale_after: 2027-01-31T00:00:00Z
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/TESTING.md", title: "TESTING.md at 0d85d00" }
---

# 3. Slurm TB nodes

The TB compute nodes run AlmaLinux 9.8 with the DAOS client and `daos_agent`, and can reach pool `sc26`.
They have `daos-devel` 2.6.5 (checked 2026-11-11 on tb14c01, tb16c01, tb18c07 and tblogin1902), but not E2SAR.
So build and run inside the Rocky 9 dev image with **podman**, or run the build from ebpf2201 natively ([without a container](#without-a-container)).

## The nodes

Checked 2026-11-11 (`sinfo`, and a 1-CPU job per node).

| Nodes | CPU, memory | Ethernet (up port) | InfiniBand (IPoIB) |
|---|---|---|---|
| tb14c01–08 | Haswell, 24 cores, 60 GB | `eno2`, Intel I350, 1 Gbps, MTU 9000 | `ibs1`, ConnectX-3, 4X FDR 56 Gbps |
| tb16c01–16 | Broadwell, 36 cores, 60 GB | `eno1`, Intel I350, 1 Gbps, MTU 9000 | `ibo1`, ConnectX-3, 4X FDR 56 Gbps |
| tb18c06 | Skylake, 40 cores, 122 GB | `eno1`, Intel X550 at 1 Gbps | `ibp94s0`, ConnectX-3, FDR 56 Gbps |
| tb18c07–08 | Skylake, 40 cores, 122 GB | `eno1`, Intel X550 at 1 Gbps | `ibs801f0`, ConnectX-5, FDR 56 Gbps (second port down) |

- Ethernet is 129.57.178.128/25 (DHCP, gateway .129), routed to the login node and ebpf2201/2203 (129.57.178.64/26). At 1 Gbps it can't take E2SAR traffic, and anything written to `$HOME` (NFS) goes through it.
- IPoIB is 172.19.27.0/24, shared with the login node and the DAOS engines. DAOS uses `ofi+verbs;ofi_rxm` over it.
- Pool `sc26` sits on two engines, ranks 4 and 5, both on daosfs2302 (`daosfs2302-ib3` 172.19.27.21 and `daosfs2302-ib1` 172.19.27.20).
- All run the DAOS 2.6.5 client and `daos_agent`. On 2026-11-11 tb16c04's agent predated the 2.6.5 upgrade and every DAOS call failed with `DER_MISC`: use `--exclude=tb16c04` until it is restarted.
- Partitions `cores` and `sockets`: 8 h default, 4 days at most, no preemption.
- No IOR. Open MPI 4.1.1 (`openmpi-devel`, `module load mpi/openmpi-x86_64`) and MPICH 4.1.1 (no devel); `srun --mpi=pmix` works. No autotools.
- Read speed of `e2sar_daos_read` from these nodes: [2026-11-11-tb-nodes-fetch](../results/2026-11-11-tb-nodes-fetch.md).
The image has its own DAOS 2.6 client and headers.
The container talks to the real DAOS system through the node's `daos_agent` socket and the node's network.

```
 TB node (AlmaLinux 9.8)
 ┌───────────────────────────────────────────────────────┐
 │  daos_agent ── /var/run/daos_agent/daos_agent.sock    │
 │       ▲                                                │
 │       │ bind mount                                     │
 │  ┌────┴─────────────────────────────┐                  │
 │  │ podman: e2sar-daos:dev (Rocky 9) │  --network host  │──► DAOS engines (sc26)
 │  │ daos_kv_test + libdaos 2.6       │                  │
 │  └──────────────────────────────────┘                  │
 └───────────────────────────────────────────────────────┘
```

The Rocky 9 userspace runs fine on the AlmaLinux 9 kernel.
On the nodes it runs natively on x86_64, with no emulation.

## Prerequisites on a TB node

```bash
srun -p <tb-partition> -N1 --pty bash      # interactive shell on a TB node

podman --version
systemctl is-active daos_agent             # active
ls -l /var/run/daos_agent/                 # daos_agent.sock is here
grep runtime_dir /etc/daos/daos_agent.yml  # if set, use that directory instead of /var/run/daos_agent below
rpm -q daos-client                         # 2.6.x; keep the container's DAOS minor version (2.6) the same
daos pool query sc26                       # the host can reach the pool as your user
```

Rootless podman keeps its image store in `~/.local/share/containers` by default.
On an NFS home directory that often fails with overlay or permission errors.
If so, use node-local storage for this session:
```bash
export CONTAINERS_STORAGE_CONF=$HOME/.config/containers/storage-tb.conf
mkdir -p $(dirname $CONTAINERS_STORAGE_CONF) /tmp/$USER/containers
cat > $CONTAINERS_STORAGE_CONF <<CONF
[storage]
driver = "overlay"
graphroot = "/tmp/$USER/containers/storage"
runroot = "/tmp/$USER/containers/run"
CONF
```
Node-local storage is wiped with the node's `/tmp`, so the image has to be loaded again (next step) on each node you use.

## Get the image onto the node

Pick one:

```bash
# (a) Build it on the node from the repo's Dockerfile (needs internet access from the node)
podman build -t e2sar-daos:dev containers/rocky-dev

# (b) Build once elsewhere, save it to the shared filesystem, load it on each node
#     On the Mac or ebpf220x:
docker save e2sar-daos:dev | gzip > /path/on/shared/fs/e2sar-daos-dev.tar.gz     # or: podman save
#     On the TB node:
podman load -i /path/on/shared/fs/e2sar-daos-dev.tar.gz
podman images e2sar-daos
```

With (a) on JLab networks, put the JLab CA certs in `containers/rocky-dev/certs/` first, as for Docker.

## Build and test inside the container

Run from the repository root on the shared filesystem.
Use a separate build directory (`build-podman`), because a build directory configured on the host or in another container has different paths and libraries.

```bash
cd /path/on/shared/fs/e2sar-daos

podman run --rm \
    --network host \
    --userns keep-id \
    --security-opt label=disable \
    -v "$PWD":/work -w /work \
    -v /var/run/daos_agent:/var/run/daos_agent \
    -e DAOS_POOL=sc26 -e DAOS_CONT=kv-test \
    e2sar-daos:dev \
    bash -c "meson setup build-podman && meson compile -C build-podman && \
             meson test -C build-podman --suite unit -v && \
             meson test -C build-podman --suite integration -v"
```

What each flag is for:

| Flag | Why |
|---|---|
| `--network host` | the DAOS client connects straight to the engines; it needs the node's interfaces, the same ones `daos_agent` reports |
| `-v /var/run/daos_agent:/var/run/daos_agent` | `daos_init()` finds the agent through this socket |
| `--userns keep-id` | the test runs as your own uid, so `daos_agent` checks the pool ACL against your user, and files in `build-podman/` are owned by you |
| `--security-opt label=disable` | SELinux would otherwise block the bind mounts. Don't use `:z` on the agent socket, because it relabels the host directory |
| `-e DAOS_POOL/DAOS_CONT` | only matters when running `daos_kv_test` directly; `meson test` sets them from the meson options |

The result looks the same as on ebpf220x (see [section 2](ebpf-testbed.md#build-and-test)): 5 matching `put`/`get` SHA-256 pairs and `Ok: 1`.

If the DAOS fabric uses InfiniBand (`verbs` or `ucx` provider) rather than TCP, the container also needs the RDMA devices and unlimited locked memory:
```bash
    --device /dev/infiniband --ulimit memlock=-1:-1 \
```
Check the provider with `grep -i provider /etc/daos/daos_agent.yml` or `daos_agent net-scan` on the node.

## Run as a batch job

`build-podman/` is on the shared filesystem, so once it's built you can run only the test from `sbatch`:

```bash
#!/bin/bash
#SBATCH -p <tb-partition>
#SBATCH -N 1
#SBATCH -t 00:10:00
#SBATCH -o daos_kv_test_%j.log

cd /path/on/shared/fs/e2sar-daos
podman run --rm --network host --userns keep-id --security-opt label=disable \
    -v "$PWD":/work -w /work \
    -v /var/run/daos_agent:/var/run/daos_agent \
    -e DAOS_POOL=sc26 -e DAOS_CONT=kv-test \
    e2sar-daos:dev ./build-podman/tests/daos/daos_kv_test --log_level=message
```

With node-local image storage, add the `CONTAINERS_STORAGE_CONF` export and `podman load` before `podman run`.

## Common failures in podman

| Message | Likely cause |
|---|---|
| `daos_init failed: ... (is daos_agent running?)` | socket not mounted, or the node's agent uses a different `runtime_dir` |
| `daos_pool_connect(sc26) failed: DER_NO_PERM` | the uid seen by the agent isn't in the pool ACL. Check `--userns keep-id`, and that `daos pool query sc26` works on the host |
| `daos_pool_connect` hangs, or `DER_UNREACH` / `DER_TIMEDOUT` | no `--network host`, or missing `--device /dev/infiniband` on an InfiniBand fabric |
| `Permission denied` on `/work` or the socket | SELinux: add `--security-opt label=disable` |
| `podman` image or overlay errors in `$HOME` | use node-local storage (see Prerequisites) |

## Without a container

Alternatively, build natively on ebpf2201 ([ebpf-testbed section 2](ebpf-testbed.md#2-ebpf2201--ebpf2203-daos-pool-and-daos-devel)) and run the binary directly on a node:
```bash
srun -p <tb-partition> -N1 -n1 env DAOS_POOL=sc26 DAOS_CONT=kv-test \
    ./build/tests/daos/daos_kv_test --log_level=message
```
At run time, `daos_kv_test` only needs `libdaos`, `libdfs`, `libgurt` and `libcrypto` from the node.
It doesn't link E2SAR, and the Boost test framework is header-only here.
This works when the node's DAOS client minor version matches ebpf2201's.
A Rocky 9 build normally runs on AlmaLinux 9, but check `ldd build/tests/daos/daos_kv_test` on the node first.

The applications, e.g. `e2sar_daos_read`, run the same way from ebpf2201's `build/`, with the Boost 1.89 libraries of the staged E2SAR on the library path:
```bash
export LD_LIBRARY_PATH=$HOME/opt/e2sar-0.4.0rc1/usr/local/lib:$LD_LIBRARY_PATH
srun -p cores -N1 -n1 --exclude=tb16c04 ~/daq/e2sar-daos/build/bin/e2sar_daos_read --pool sc26 --cont <run> --list
```

---
