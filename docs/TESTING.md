# Testing e2sar-daos

There are two test suites:

| Suite | Test | Needs |
|---|---|---|
| `unit` | `DAOSNativeTests`: checks that `<daos.h>` compiles and links, and the DAOS major version is 2.<br>`EventLayoutTests`: checks the key names and 1 MiB chunking that `e2sar_daos_recv`/`e2sar_daos_read` use | DAOS headers and libraries, no DAOS server |
| `integration` | `DAOSKVTest`: writes 5 random 0.5 MiB values into a DAOS KV object, reads them back and compares SHA-256 digests | A reachable DAOS pool and container, plus `openssl-devel` at build time |

`DAOSKVTest` reads the pool and container from `DAOS_POOL` and `DAOS_CONT` (defaults `sc26` and `kv-test`).
`meson test` sets both from the meson options `daos_pool` and `daos_cont`.

What each environment can check:

| Environment | Build | `unit` | `integration` |
|---|---|---|---|
| [Mac (no DAOS)](#1-mac-without-daos) | in the Docker container | yes | no |
| [ebpf2201 / ebpf2203](#2-ebpf2201--ebpf2203-daos-pool-and-daos-devel) | natively | yes | yes |
| [Slurm TB nodes](#3-slurm-tb-nodes-daos-pool-but-no-daos-devel) | in a podman container (or on ebpf220x) | yes | yes |

The applications (`e2sar_daos_recv`, `e2sar_daos_read`) are tested end to end by hand. See [section 4](#4-applications-end-to-end-on-ebpf2203).

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
- `--suite unit` reports `Ok: 2`.

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

The TB compute nodes run AlmaLinux 9.8 with the DAOS client and `daos_agent`, and can reach pool `sc26`.
They don't have `daos-devel`, so the project can't be built on the host: the top-level `meson.build` looks for DAOS headers in `/usr/include`.
Instead, build and run inside the Rocky 9 dev image with **podman**.
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

### Prerequisites on a TB node

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

### Get the image onto the node

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

### Build and test inside the container

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

The result looks the same as on ebpf220x (see [section 2](#build-and-test)): 5 matching `put`/`get` SHA-256 pairs and `Ok: 1`.

If the DAOS fabric uses InfiniBand (`verbs` or `ucx` provider) rather than TCP, the container also needs the RDMA devices and unlimited locked memory:
```bash
    --device /dev/infiniband --ulimit memlock=-1:-1 \
```
Check the provider with `grep -i provider /etc/daos/daos_agent.yml` or `daos_agent net-scan` on the node.

### Run as a batch job

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

### Common failures in podman

| Message | Likely cause |
|---|---|
| `daos_init failed: ... (is daos_agent running?)` | socket not mounted, or the node's agent uses a different `runtime_dir` |
| `daos_pool_connect(sc26) failed: DER_NO_PERM` | the uid seen by the agent isn't in the pool ACL. Check `--userns keep-id`, and that `daos pool query sc26` works on the host |
| `daos_pool_connect` hangs, or `DER_UNREACH` / `DER_TIMEDOUT` | no `--network host`, or missing `--device /dev/infiniband` on an InfiniBand fabric |
| `Permission denied` on `/work` or the socket | SELinux: add `--security-opt label=disable` |
| `podman` image or overlay errors in `$HOME` | use node-local storage (see Prerequisites) |

### Without a container

Alternatively, build natively on ebpf2201 (section 2) and run the binary directly on a node:
```bash
srun -p <tb-partition> -N1 -n1 env DAOS_POOL=sc26 DAOS_CONT=kv-test \
    ./build/tests/daos/daos_kv_test --log_level=message
```
At run time, `daos_kv_test` only needs `libdaos`, `libdfs`, `libgurt` and `libcrypto` from the node.
It doesn't link E2SAR, and the Boost test framework is header-only here.
This works when the node's DAOS client minor version matches ebpf2201's.
A Rocky 9 build normally runs on AlmaLinux 9, but check `ldd build/tests/daos/daos_kv_test` on the node first.

---

## 4. Applications end to end on ebpf2203

This checks the full path from EVIO files back to EVIO files:

```
EVIO files ─ e2sar_ft -s ─UDP─▶ e2sar_daos_recv ─async KV puts─▶ sc26/e2sar-test ─▶ e2sar_daos_read -o ─▶ .evio files ─▶ viewer
```

It runs on one host over loopback, with no load balancer or control plane. Then it repeats with the sender on ebpf2201.
Build first as in [section 2](#2-ebpf2201--ebpf2203-daos-pool-and-daos-devel).
`meson setup` must not print `skipping e2sar_daos_read`.
The test uses a dedicated container, `e2sar-test`, so it can be removed afterwards. `e2sar_daos_recv` creates it.

### Setup (both terminals)

```bash
cd e2sar-daos
E2SAR_BIN=$E2SAR_ROOT/usr/local/bin        # where e2sar_ft is; adjust to your E2SAR install
# if a binary can't find libe2sar.so:
# export LD_LIBRARY_PATH=$E2SAR_ROOT/usr/local/lib64:$E2SAR_ROOT/usr/local/lib

# Without a control plane only data= matters. The sender uses data port 19522.
export EJFAT_URI='ejfat://useless@127.0.0.1:18347/lb/1?data=127.0.0.1&sync=127.0.0.1:19020'
export PREFIX=test_$(date +%s)              # same value in both terminals
```

Source files: a few EVIO files under 2 GiB (e.g. `EC5_EC6_cosmics_may19_short.evio`, `frames_thread0_file0000.part000.evio`), plus one random file. The random file's size is not a multiple of the 1 MiB chunk, and it should come back as `not EVIO`.
```bash
SRC=/tmp/$USER/e2sar_src && mkdir -p $SRC && rm -f $SRC/*
cp /path/to/*.evio $SRC/
head -c 3500K /dev/urandom > $SRC/random_3500K.bin
sha256sum $SRC/* | tee /tmp/$USER/src.sha256
```

### Run

**Terminal 1, receiver:**
```bash
./build/bin/e2sar_daos_recv --ip 127.0.0.1 --port 19522 \
    --pool sc26 --cont e2sar-test --prefix $PREFIX --verbose
```
On the first run it prints `DAOS pool/container: sc26/e2sar-test (created)`, then a progress line every second.

**Terminal 2, sender:**
```bash
$E2SAR_BIN/e2sar_ft -s --ip 127.0.0.1 --rate 1 $SRC/*
```
The receiver prints one `committed <PREFIX>/<evt>/4321 (<bytes> bytes)` line per file. 4321 is `e2sar_ft`'s default data id.
When every file is in, press Ctrl-C in terminal 1.
The final stats should show `Events committed` equal to the number of files, `Events failed: 0` and `Lost in reassembly: 0`.

### Verify and retrieve as EVIO

```bash
# List: one row per file, chunks = ceil(size / 1 MiB)
./build/bin/e2sar_daos_read --cont e2sar-test --prefix $PREFIX

# Read back, check, and restore as <PREFIX>_<evt>_4321.evio
OUT=/tmp/$USER/e2sar_out && mkdir -p $OUT
./build/bin/e2sar_daos_read --cont e2sar-test --prefix $PREFIX --fetch -o $OUT | tee /tmp/$USER/daos.sha256
```
Each line reads like `<sha256>  test_..._0_4321.evio  (93456789 bytes, 90 chunks, EVIO v6 big-endian)`.
The EVIO files should say `EVIO v6`, and the random file `not EVIO`.

```bash
# Same set of digests as the source files
diff <(cut -d' ' -f1 /tmp/$USER/src.sha256 | sort) \
     <(grep -E '^[0-9a-f]{64} ' /tmp/$USER/daos.sha256 | cut -d' ' -f1 | sort) && echo PASS
```

The restored files don't keep their original names (E2SAR carries no file name), only event numbers.
`e2sar_ft` numbers them in the order it queued the files. Match them by digest: `sha256sum $OUT/*` against `/tmp/$USER/src.sha256`.

**Open in the viewer.** Run evio-interactive-view on ebpf2203 and tunnel to it from your laptop:
```bash
# on ebpf2203, in evio-interactive-view with its venv active
python serve.py $OUT --no-browser
# on the laptop
ssh -L 8765:localhost:8765 ebpf2203     # then open http://localhost:8765
```
Or copy the files to the laptop (`scp 'ebpf2203:/tmp/<user>/e2sar_out/*.evio' .`) and open them there.

### Across hosts (the real setup)

Sender on ebpf2201, receiver and DAOS on ebpf2203. Use the ebpf2203 data-plane IP in both the URI and `--ip`:
```bash
# both hosts
export EJFAT_URI='ejfat://useless@127.0.0.1:18347/lb/1?data=<ebpf2203 ip>&sync=<ebpf2203 ip>:19020'
# ebpf2203
./build/bin/e2sar_daos_recv --ip <ebpf2203 ip> --port 19522 --pool sc26 --cont e2sar-test --prefix $PREFIX
# ebpf2201
$E2SAR_BIN/e2sar_ft -s --ip <ebpf2201 ip> --rate 5 $SRC/*
```
Then verify on ebpf2203 as above. Firewalls between the two hosts must let UDP 19522 through.

**Variations worth running:**
- Throughput and back-pressure: larger EVIO files, `--rate 10`, and `--deq 2 --inflight 128` on the receiver. Watch `enqueue loss` in the progress line.
- Single-value events: `--chunk 0` on the receiver. The listing then shows 1 chunk per event.

### Clean up

```bash
daos cont destroy sc26 e2sar-test
rm -rf /tmp/$USER/e2sar_src /tmp/$USER/e2sar_out /tmp/$USER/*.sha256
```

### Common failures

| Symptom | Likely cause |
|---|---|
| receiver shows nothing, sender says frames were sent | port mismatch: the receiver needs `--port 19522`, or put `data=<ip>:<port>` in the URI. Across hosts, also check the firewall |
| `Lost in reassembly` > 0 | UDP drops. Lower the sender `--rate`, or raise `-b` and `net.core.rmem_max` |
| `daos_cont_create_with_label ... DER_NO_PERM` | your user can't create containers in `sc26`. Ask for pool ACL `w`, or create it with `daos cont create sc26 e2sar-test` |
| `e2sar_daos_read` lists 0 events | wrong `--prefix`/`--cont`/`--oid`, or the receiver was killed before commit (look for *Incomplete*) |
| viewer refuses a file | the viewer only takes files < 2 GiB. Split the source with `evio_split.py` before sending |
