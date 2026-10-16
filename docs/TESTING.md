# Testing e2sar-daos

There are two test suites:

| Suite | Test | Needs |
|---|---|---|
| `unit` | `DAOSNativeTests`: checks that `<daos.h>` compiles and links, and the DAOS major version is 2 | DAOS headers and libraries, no DAOS server |
| `integration` | `DAOSKVTest`: writes 5 random 0.5 MiB values into a DAOS KV object, reads them back and compares SHA-256 digests | A reachable DAOS pool and container, plus `openssl-devel` at build time |

`DAOSKVTest` reads the pool and container from `DAOS_POOL` and `DAOS_CONT` (defaults `sc26` and `kv-test`).
`meson test` sets both from the meson options `daos_pool` and `daos_cont`.

What each environment can check:

| Environment | Build | `unit` | `integration` |
|---|---|---|---|
| [Mac (no DAOS)](#1-mac-without-daos) | in the Docker container | yes | no |
| [ebpf2201 / ebpf2203](#2-ebpf2201--ebpf2203-daos-pool-and-daos-devel) | natively | yes | yes |
| [Slurm TB nodes](#3-slurm-tb-nodes-daos-pool-but-no-daos-devel) | on ebpf220x, or in the container | yes | yes |

---

## 1. Mac without DAOS

DAOS and E2SAR only ship x86_64 Linux packages, so on a Mac the build runs in the Rocky 9 dev container ([containers/rocky-dev](../containers/rocky-dev/)) under `linux/amd64` emulation.
That container has DAOS 2.6 client libraries and headers and a prebuilt E2SAR, but no DAOS server.
So it checks that everything compiles and links, and runs the `unit` suite.
It cannot run `DAOSKVTest`.

```bash
cd e2sar-daos

# Build the image (once, or after the Dockerfile changes).
# On JLab networks, first add the JLab CA certs to containers/rocky-dev/certs/.
docker build --platform linux/amd64 -t e2sar-daos:dev containers/rocky-dev

# Configure, compile and run the unit suite.
# The build dir is inside the container (/tmp/b) so it doesn't clash with a host build/.
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work e2sar-daos:dev \
    bash -c "meson setup /tmp/b && meson compile -C /tmp/b && meson test -C /tmp/b --suite unit -v"
```

Check that:
- `meson setup` does **not** print `libcrypto (openssl-devel) not found, skipping DAOSKVTest`, so the KV test is at least compiled.
- `meson compile` links both `daos_test` and `daos_kv_test`.
- `--suite unit` reports `Ok: 1`.

If `meson setup` fails with `Could not generate cargs for e2sar` or `Package 're2' ... not found`, the image was built from an older Dockerfile. Rebuild it.

---

## 2. ebpf2201 / ebpf2203 (DAOS pool and daos-devel)

These hosts have the DAOS client, `daos_agent` and `daos-devel`, so both suites run natively.

### Prerequisites (once)

```bash
rpm -q daos-client daos-devel openssl-devel   # all three must be installed
systemctl is-active daos_agent                # must be "active"
daos pool query sc26                          # pool sc26 is reachable by you
daos cont create sc26 kv-test                 # only if the container doesn't exist yet
daos cont query sc26 kv-test                  # container is reachable
```

E2SAR must be installed and visible to `pkg-config` (see [Point pkg-config to E2SAR](../README.md#point-pkg-config-to-e2sar)):
```bash
pkg-config --modversion e2sar
```

### Build and test

Use a fresh clone (or `git worktree`) of the branch under test, so it doesn't mix with uncommitted work:
```bash
git clone -b <branch> git@github.com:JeffersonLab/e2sar-daos.git && cd e2sar-daos

meson setup build
meson compile -C build
meson test -C build --suite unit -v
meson test -C build --suite integration -v
```

A passing `DAOSKVTest` prints a `put` and a `get` line with the same SHA-256 for each of the 5 keys, and meson reports `Ok: 1`:
```
put 3f1c...e9  toy_12345_1760640000_0
...
get 3f1c...e9  toy_12345_1760640000_0
```
The test removes its keys at the end, so the container doesn't grow across runs.

### Another pool or container

```bash
meson configure build -Ddaos_pool=mypool -Ddaos_cont=mycont
meson test -C build --suite integration -v

# or run the binary directly
DAOS_POOL=mypool DAOS_CONT=mycont ./build/tests/daos/daos_kv_test --log_level=message
```

### Common failures

| Message | Likely cause |
|---|---|
| `daos_init failed: ... (is daos_agent running?)` | `daos_agent` is not running on this host |
| `daos_pool_connect(sc26) failed: DER_NO_PERM` | your user is not in the pool ACL |
| `daos_pool_connect(sc26) failed: DER_NONEXIST` | wrong pool label, or the pool is in another DAOS system |
| `daos_cont_open(kv-test) failed: DER_NONEXIST` | run `daos cont create sc26 kv-test` |

---

## 3. Slurm TB nodes (DAOS pool but no daos-devel)

The TB compute nodes can reach pool `sc26` (DAOS client and `daos_agent` installed) but don't have `daos-devel`, so they can't **build** this project.
The top-level `meson.build` looks for DAOS headers in `/usr/include` only.
Build somewhere else and run on the node.
The test binaries only need these shared libraries at run time:

```bash
ldd build/tests/daos/daos_kv_test | grep -E 'daos|dfs|crypto'
# libdaos.so.2, libdfs.so, libcrypto.so.3 — all present on a node with daos-client
```

`DAOSKVTest` does not link E2SAR, and the Boost test framework is compiled in from headers, so neither needs to be installed on the node.

### Option A (recommended): build on ebpf220x, run with srun

This works when ebpf220x and the TB nodes run the same OS (Rocky 9) and DAOS 2.6 client, and share a filesystem (e.g. `$HOME` or `/work`).
Check both with `cat /etc/rocky-release; rpm -q daos-client` on each side.

```bash
# On ebpf2201: build in a directory the TB nodes can see
cd /path/on/shared/fs/e2sar-daos
meson setup build && meson compile -C build

# Submit to a TB node. Adjust partition/account to the testbed's Slurm setup.
srun -p <tb-partition> -N1 -n1 --time=00:05:00 \
    env DAOS_POOL=sc26 DAOS_CONT=kv-test \
    ./build/tests/daos/daos_kv_test --log_level=message
```

Use the binary directly here instead of `meson test`, because the node may not have meson.
For a batch job, put the same command in an `sbatch` script.

Also run the `unit` binary on the node to confirm the node's `libdaos` matches what you built against:
```bash
srun -p <tb-partition> -N1 -n1 ./build/tests/daos/daos_test
```

### Option B: build and run in the container with Apptainer

Use this when the node's OS or library versions differ from ebpf220x.
The dev image has its own DAOS 2.6 client and headers.
The node's `daos_agent` is shared with the container through its socket directory, so the container talks to the real DAOS system.

```bash
# Once, on a host with Docker: export the image, copy it to the shared filesystem
docker save e2sar-daos:dev -o e2sar-daos-dev.tar
apptainer build e2sar-daos-dev.sif docker-archive://e2sar-daos-dev.tar

# Build inside the container (any node or ebpf220x)
apptainer exec --bind "$PWD":/work --pwd /work e2sar-daos-dev.sif \
    bash -c "meson setup build-sif && meson compile -C build-sif"

# Run on a TB node, sharing the host's daos_agent socket
srun -p <tb-partition> -N1 -n1 \
    apptainer exec --bind "$PWD":/work --pwd /work --bind /var/run/daos_agent \
    --env DAOS_POOL=sc26,DAOS_CONT=kv-test \
    e2sar-daos-dev.sif ./build-sif/tests/daos/daos_kv_test --log_level=message
```

Notes:
- `/var/run/daos_agent` is the default agent socket directory. If the node's `/etc/daos/daos_agent.yml` sets a different `runtime_dir`, bind that one instead.
- Keep the container's DAOS client minor version (2.6) the same as the servers'.
- Use a separate build directory (`build-sif`) from the native build, since they link different libraries.
