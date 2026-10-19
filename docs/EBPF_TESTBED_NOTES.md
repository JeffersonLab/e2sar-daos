# Testing on ebpf2201 and ebpf2203

This is the procedure for testing `e2sar_ft` → `e2sar_daos_recv` → DAOS → `e2sar_daos_read` on the ebpf testbed.
Both nodes are DAOS clients of pool `sc26`, so either one can be the receiver. The steps use ebpf2201 as the sender and ebpf2203 as the receiver, and step 6 shows the reverse.
Every run uses **MTU 9000**. The sender reads from local `/scratch/<user>`, and the receiver restores into local `/scratch/<user>`.
Last run on 2026-10-19 against PR #11 (`feature/daos-kv-recv-apps`). Settings that differ from [TESTING.md §4](TESTING.md#4-applications-end-to-end-on-ebpf2203) are marked **⚠**.
For throughput limits, see [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md). To send through the ESnet EJFAT load balancer instead, see [EJFAT_LB_TESTING.md](EJFAT_LB_TESTING.md).

```
ebpf2201                                         ebpf2203
/scratch/<user>/e2sar_src                        DAOS sc26/e2sar-test ──▶ /scratch/<user>/e2sar_out
  e2sar_ft -s --mtu 9000 ──UDP :20480──▶ e2sar_daos_recv
129.57.178.85                                    129.57.178.86
```

## 0. What the nodes provide

| | ebpf2201 | ebpf2203 |
|---|---|---|
| E2SAR 0.4.0rc1 in `~/opt/e2sar` (`e2sar_ft`) | yes | yes |
| e2sar-daos build (`~/daq/e2sar-daos/build`) | shared `$HOME` | shared `$HOME` |
| DAOS 2.6.5 client and running `daos_agent`, pool `sc26` | yes | yes |
| Data-plane NIC `enp1s0f1np1` | 100 Gb/s, MTU 9000, 129.57.178.85 | 100 Gb/s, MTU 9000, 129.57.178.86 |
| `/scratch` | local XFS, ~880 GB | local XFS, ~740 GB |

- `$HOME` (`/testbed/home/<user>`) is an NFS mount shared by both nodes. It holds the build, the scripts and the source digests.
- `/scratch/<user>` is **local to each node**. Each sender needs its own copy of the source files, and each receiver writes its own restored files.
- The login shell is tcsh. Run every snippet below inside `bash`.
- `meson` and `ninja` are in `~/.local/bin`.
- **⚠** ebpf2203 also has the e2sar 0.3.1 RPM, which installs `e2sar_ft` in `/usr/local/bin`. Always call the 0.4.0rc1 binary as `$E2SAR_BIN/e2sar_ft`.
- This E2SAR build has no NUMA support. `--numa` makes both apps exit with `NUMA management not available on this system`.
- GitHub over SSH is blocked from the nodes. Use HTTPS to clone or pull.

## 1. Environment (each shell, both nodes)

```bash
bash
export PATH=$HOME/.local/bin:$PATH
export E2SAR_ROOT=$HOME/opt/e2sar
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH
export E2SAR_BIN=$E2SAR_ROOT/usr/local/bin
export PORT=20480                      # ⚠ data port; must appear in EJFAT_URI
export SCR=/scratch/$USER              # node-local
export SRC=$SCR/e2sar_src              # sender input
export OUT=$SCR/e2sar_out              # receiver output
export RUN=$HOME/e2sar_run             # shared: digests and logs
```

## 2. Build (once, on either node)

```bash
cd ~/daq/e2sar-daos
git pull https://github.com/JeffersonLab/e2sar-daos.git feature/daos-kv-recv-apps   # SSH to GitHub is blocked
meson setup build                          # "Boost ... found: YES 1.89.0 (.../opt/e2sar/usr/local)"
meson compile -C build
meson test -C build --suite unit           # Ok: 3, Fail: 0
```

## 3. Source files (before each run, on the sending node)

Step 8 deletes the source copies after each run, so stage them again for every run.

Keep each file under 2 GiB so the EVIO viewer can open it. `frames_thread0_file0020.evio` (1.78 GB) is the one file in `~/evio-data` that qualifies.
Add one random file whose size is not a multiple of 1 MiB. Generate it once and copy that same file to each sender, so the digests stay the same.

```bash
mkdir -p $SRC $RUN
cp $HOME/evio-data/frames_thread0_file0020.evio $SRC/
[ -f $RUN/random_3500K.bin ] || head -c 3500K /dev/urandom > $RUN/random_3500K.bin
cp $RUN/random_3500K.bin $SRC/
(cd $SRC && sha256sum -- *) | tee $RUN/src.sha256      # identical on every node
```

## 4. Loopback on ebpf2203

Run this first. It checks the build and DAOS without the network.

```bash
export EJFAT_URI="ejfat://useless@127.0.0.1:18347/lb/1?data=127.0.0.1:$PORT&sync=127.0.0.1:19020"
export PREFIX=test_$(date +%s)                 # same value in both terminals
```

**Terminal 1, receiver:**
```bash
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_recv --ip 127.0.0.1 --port $PORT \
    --pool sc26 --cont e2sar-test --prefix $PREFIX \
    --timeout 10000 -b 1073741824 --verbose
```

**Terminal 2, sender:**
```bash
$E2SAR_BIN/e2sar_ft -s --ip 127.0.0.1 --mtu 9000 --rate 5 -p $SRC/*
```

Expected: the sender prints `Queueing file` once per file and `0 errors`. The receiver prints one `committed <PREFIX>/<evt>/4321` line per file.
Press Ctrl-C in terminal 1 after the last `committed` line. The final stats should show `Lost in reassembly: 0` and `Events failed: 0`.
Then verify (step 7) and clean up (step 8).

## 5. Across hosts: ebpf2201 → ebpf2203

Both hosts use the **receiver's** IP in the URI. Set the same `PREFIX` value on both hosts.

```bash
export EJFAT_URI="ejfat://useless@127.0.0.1:18347/lb/1?data=129.57.178.86:$PORT&sync=129.57.178.86:19020"
export PREFIX=x_<same timestamp on both hosts>
```

**ebpf2203, receiver** (start it first):
```bash
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_recv --ip 129.57.178.86 --port $PORT \
    --pool sc26 --cont e2sar-test --prefix $PREFIX \
    --timeout 10000 -b 1073741824 --verbose
```
It should print `Listening on: 129.57.178.86 ports 20480:20480`.

**ebpf2201, sender:**
```bash
ip route get 129.57.178.86        # expect: dev enp1s0f1np1 src 129.57.178.85
$E2SAR_BIN/e2sar_ft -s --ip 129.57.178.85 --mtu 9000 --rate 5 -p $SRC/*
```
Ctrl-C the receiver after the last `committed` line, then verify on ebpf2203 (step 7) and clean up both nodes (step 8).

## 6. Reverse: ebpf2203 → ebpf2201

This is the same as step 5 with the roles swapped. Use 129.57.178.85 in the URI and as the receiver's `--ip`, and run `e2sar_ft` on ebpf2203 with `--ip 129.57.178.86`.
ebpf2203 needs its own `$SRC` from step 3, and the restored files land in ebpf2201's `/scratch`.

## 7. Verify and restore (on the receiver)

```bash
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_read --cont e2sar-test --prefix $PREFIX            # one row per file
mkdir -p $OUT/$PREFIX
./build/bin/e2sar_daos_read --cont e2sar-test --prefix $PREFIX --fetch -o $OUT/$PREFIX | tee $OUT/$PREFIX.sha256
diff <(cut -d' ' -f1 $RUN/src.sha256 | sort) \
     <(grep -E '^[0-9a-f]{64} ' $OUT/$PREFIX.sha256 | cut -d' ' -f1 | sort) && echo PASS
```
The EVIO file should show `EVIO v6 big-endian`, and the random file `not EVIO`.
Restored files are named `<PREFIX>_<evt>_4321.evio`. To view them, see the viewer part of TESTING.md §4.

## 8. Clean up after every run (required)

Each run leaves gigabytes behind: the source copy on the sender, the restored files on the receiver, and the event data in DAOS.
Remove all three when the run is verified, before starting the next one. Keep `$RUN`, which holds the logs, digests and `random_3500K.bin`.

```bash
# receiver: restored files and this run's DAOS data
rm -rf $OUT/$PREFIX $OUT/$PREFIX.sha256
daos cont destroy sc26 e2sar-test               # e2sar_daos_recv recreates it on the next run
# sender: source copies
rm -rf $SRC
```

`daos cont destroy` removes every prefix in the container. Make sure no other run is using `e2sar-test` at the same time.
Check that nothing is left: run `ls /scratch/$USER` on both nodes, and `daos cont list sc26` should not show `e2sar-test`.

## Results from 2026-10-19 (MTU 9000, `--rate 5`, `-b 1 GiB`)

Each run sent `frames_thread0_file0020.evio` (1.78 GB) and a 3.5 MB random file. Every run committed 2 of 2 events with 0 lost, 0 kernel `RcvbufErrors` and matching digests.

| Run | Rep | Sender goodput | Ingest | DAOS commit |
|---|---|---|---|---|
| Loopback on ebpf2203 | 1 | 4.2 Gbps | 4.7 Gbps | 4.5 Gbps |
| | 2 | 7.6 Gbps | 9.2 Gbps | 8.5 Gbps |
| ebpf2201 → ebpf2203 | 1 | 7.4 Gbps | 8.9 Gbps | 8.3 Gbps |
| | 2 | 7.3 Gbps | 8.8 Gbps | 8.2 Gbps |
| ebpf2203 → ebpf2201 | 1 | 14.3 Gbps | 20.9 Gbps | 17.8 Gbps |
| | 2 | 11.1 Gbps | 9.0 Gbps | 8.4 Gbps |

These runs read the source from `$HOME` (NFS). The `/scratch` runs in the bottleneck analysis gave the same range, so reading from local disk is for consistency, not speed.

## Recommended settings

| | Setting | Why |
|---|---|---|
| Sender | `--mtu 9000` | At 1500, ~2 Gbps maximum |
| Sender | `--rate 5` (any value ≥ 5 behaves the same) | `--rate` only spaces out events; see the analysis |
| Sender | no `--smooth` | It caps the sender below 1 Gbps, too slow for a 1.78 GB event in 10 s |
| Sender | `-p $SRC/*` | Required after list-valued options such as `-o`, which otherwise swallow the file names |
| Receiver | `-b 1073741824` | With 256 MiB the kernel dropped packets in 3 of 11 runs. With 1 GiB it dropped none in 20 runs |
| Receiver | `--timeout 10000` | The maximum. The default 500 ms drops multi-GB events |
| Both | `data=<ip>:$PORT` in `EJFAT_URI` | Without the port, `e2sar_ft` sends to junk ports |

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Receiver gets nothing. Sender exits 0 but shows `N errors` / `Connection refused` | **⚠** `EJFAT_URI` has `data=<ip>` with no port. `e2sar_ft` 0.4.0rc1 then sends to ports 16384/20480/24576/28672. Write `data=<ip>:$PORT`. To confirm, run `strace -f -e trace=connect $E2SAR_BIN/e2sar_ft ...` |
| Sender prints `0 frames sent, 0 errors` and no `Queueing file` lines, exit 0 | The source path doesn't exist on **this** node (`/scratch` is local). `e2sar_ft` reports no error for a missing path |
| Sender exits 255: `Option 'send' requires option 'path'` | A list-valued option (`-o sendmmsg`, `--cores ...`) swallowed the file names. Pass files with `-p` |
| Sender exits 255: `Conflicting options 'cores' and 'sockets'` | With `--cores`, the number of send threads is the number of cores. Drop `--sockets` |
| Large file missing, `Lost in reassembly: 1`, `RcvbufErrors` 0 | The event wasn't complete within `--timeout`. Use `--timeout 10000`, `--mtu 9000`, and no `--smooth` |
| Large file missing, `RcvbufErrors` > 0 | The receive socket overflowed. Use `-b 1073741824` |
| Receiver exits: `Event timeout exception unreasonably long, limit 10s` | `--timeout` is capped at 10000 ms |
| Either app exits: `NUMA management not available` | This build has no NUMA support. Drop `--numa` |
| Receiver exits at once and the sender shows `Connection refused` | Read the receiver output. Check that it is still running before starting the sender |

## Appendix: remote access from a laptop

The route is laptop → scilogin → tblogin → ebpf220x. scilogin asks for a password and MFA, so scripted SSH, including tools such as Claude Code, can't log in by itself.
Share one interactive login per host with SSH connection sharing:

```
# ~/.ssh/config
Host scilogin tblogin ebpf* daos-adm
  ControlMaster auto
  ControlPath ~/.ssh/cm/%r@%h:%p
  ControlPersist 12h
```

```bash
mkdir -p ~/.ssh/cm && chmod 700 ~/.ssh/cm
ssh -fN ebpf2201 && ssh -fN ebpf2203        # log in once
ssh -O check ebpf2201                          # "Master running"
ssh ebpf2203 bash -s < script.sh               # later commands reuse the login; bash because the login shell is tcsh
```
