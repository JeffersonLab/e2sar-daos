---
type: Site
title: "ebpf2201 / ebpf2203 testbed"
description: "The JLab ebpf nodes (pool sc26, native daos-devel): prerequisites, build and test suites, and the applications end to end over loopback and across the two nodes."
tags: [ebpf-testbed, sc26, testing]
stale_after: 2026-10-29T23:15:16Z
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/TESTING.md", title: "TESTING.md at 0d85d00" }
---

# ebpf2201 / ebpf2203 testbed

> **Stale since layout v2 ([#31](https://github.com/JeffersonLab/e2sar-daos/pull/31)):** section 4 still uses `--prefix`, `--prefixes` or `--oid`, which `e2sar_daos_recv` and `e2sar_daos_read` no longer have. A run is now one container (`--cont-name` / `--cont`). Update and rerun before relying on them ([#26](https://github.com/JeffersonLab/e2sar-daos/issues/26)).

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

E2SAR must be installed and visible to `pkg-config`, and Meson must use the Boost installed with E2SAR, not the system one in `/usr/include/boost`.
See [Point pkg-config to E2SAR](../howto/build.md#point-pkg-config-and-meson-to-e2sar) and [Point Meson to E2SAR's Boost](../howto/build.md#point-pkg-config-and-meson-to-e2sar).
Put this in every shell you build or run in:
```bash
E2SAR_ROOT=$HOME/opt/e2sar                  # adjust to your E2SAR staging root
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH

pkg-config --modversion e2sar               # sanity check
```

### Build and test

Use a fresh clone (or `git worktree`) of the branch under test, so it doesn't mix with uncommitted work:
```bash
git clone -b <branch> git@github.com:JeffersonLab/e2sar-daos.git && cd e2sar-daos

meson setup build                           # prints "Boost version: ..."; must be E2SAR's Boost
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
| `meson setup` finds a different Boost version than E2SAR's, or link errors in `boost::log::v2_mt_posix` / `boost::urls` | `BOOST_ROOT` is unset, so Meson used the system Boost. Export it as above and run `meson setup --wipe build` |
| a binary fails with `libboost_*.so... cannot open shared object file` | `LD_LIBRARY_PATH` doesn't include `$BOOST_ROOT/lib64` |
| `daos_pool_connect(sc26) failed: DER_NO_PERM` | your user is not in the pool ACL |
| `daos_pool_connect(sc26) failed: DER_NONEXIST` | wrong pool label, or the pool is in another DAOS system |
| `daos_cont_open(kv-test) failed: DER_NONEXIST` | run `daos cont create sc26 kv-test` |

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
# plus the E2SAR_ROOT / PKG_CONFIG_PATH / BOOST_ROOT / LD_LIBRARY_PATH exports from section 2
E2SAR_BIN=$E2SAR_ROOT/usr/local/bin        # where e2sar_ft is; adjust to your E2SAR install

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
    --pool sc26 --cont-name e2sar-test --prefix $PREFIX --verbose
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
./build/bin/e2sar_daos_recv --ip <ebpf2203 ip> --port 19522 --pool sc26 --cont-name e2sar-test --prefix $PREFIX
# ebpf2201
$E2SAR_BIN/e2sar_ft -s --ip <ebpf2201 ip> --rate 5 $SRC/*
```
Then verify on ebpf2203 as above. Firewalls between the two hosts must let UDP 19522 through.

**Variations worth running:**
- Throughput: send a few GB (tens of files or more) at increasing sender `--rate`, with `--timing-csv /tmp/$USER/timing.csv` on the receiver. Compare *Ingest* with *DAOS commit throughput* in the `Timing:` block at exit. While they match, DAOS keeps up. When commit falls behind and `enqueue loss` grows, try `--deq 2 --inflight 128`. See [bin/README.md, Timing and throughput](../reference/e2sar_daos_recv.md#timing-and-throughput).
- Single-value events: `--chunk 0` on the receiver. The listing then shows 1 chunk per event.

### Clean up (after every run)

Clean up when each run is verified, before starting the next one. Each run leaves the source copies, the restored files and the event data in DAOS behind.

```bash
# receiver (ebpf2203): restored files, digests and this run's DAOS data
rm -rf /tmp/$USER/e2sar_out /tmp/$USER/*.sha256
daos cont destroy sc26 e2sar-test            # e2sar_daos_recv recreates it on the next run
# sender (ebpf2203 for loopback, ebpf2201 across hosts): source copies
rm -rf /tmp/$USER/e2sar_src
```
`/tmp` is local to each node, so run the sender line on the node that sent.
`daos cont destroy` removes every prefix in `e2sar-test`, so make sure no other run is using it. Don't destroy `kv-test`: the integration test uses it.
To check, run `ls /tmp/$USER` on both nodes, and `daos cont list sc26` should not show `e2sar-test`.

### Common failures

| Symptom | Likely cause |
|---|---|
| receiver shows nothing, sender says frames were sent | port mismatch: the receiver needs `--port 19522`, or put `data=<ip>:<port>` in the URI. Across hosts, also check the firewall |
| `Lost in reassembly` > 0 | UDP drops. Lower the sender `--rate`, or raise `-b` and `net.core.rmem_max` |
| `daos_cont_create_with_label ... DER_NO_PERM` | your user can't create containers in `sc26`. Ask for pool ACL `w`, or create it with `daos cont create sc26 e2sar-test` |
| `e2sar_daos_read` lists 0 events | wrong `--prefix` or `--cont` (`--prefixes` lists the runs in a container), wrong `--oid` for data in the shared object, or the receiver was killed before commit (look for *Incomplete*) |
| viewer refuses a file | the viewer only takes files < 2 GiB. Split the source with `evio_split.py` before sending |

---
