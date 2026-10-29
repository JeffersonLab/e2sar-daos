# Testing on ebpf2201 and ebpf2203

This is the procedure for testing `e2sar_ft` → `e2sar_daos_recv` → DAOS → `e2sar_daos_read` on the ebpf testbed.
Both nodes are DAOS clients of pool `sc26`, so either one can be the receiver. The steps use ebpf2201 as the sender and ebpf2203 as the receiver, and step 6 shows the reverse.
Every run uses **MTU 9000**. The sender reads from local `/scratch/<user>`, and the receiver restores into local `/scratch/<user>`.
Last run on 2026-10-19 against PR #11 (`feature/daos-kv-recv-apps`). Settings that differ from [TESTING.md §4](TESTING.md#4-applications-end-to-end-on-ebpf2203) are marked **⚠**.
For throughput limits, see [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md). To send through the ESnet EJFAT load balancer instead, see [ESNET_LB_TESTING.md](ESNET_LB_TESTING.md).
Writing events as files through dfuse was tested and then removed; the results are in [DFUSE_TESTING.md](DFUSE_TESTING.md).
For throughput and latency with synthetic EVIO frames, with or without the LB, see [E2E_BENCHMARK.md](E2E_BENCHMARK.md).

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
- `e2sar_ft` is not on `PATH`. Call it as `$E2SAR_BIN/e2sar_ft`.
- This E2SAR build has no NUMA support. `--numa` makes both apps exit with `NUMA management not available on this system`.
- GitHub over SSH is blocked from the nodes. Use HTTPS to clone or pull.
- For parallel senders (step 5b), set the receiver's NIC RX ring to its maximum. It helps but doesn't prevent loss at 4 or more senders. See [NIC RX ring](#nic-rx-ring-receiver-needs-root) below.

### NIC RX ring (receiver, needs root)

The default RX ring is 1024 entries per queue. With 4 or more parallel senders, the NIC dropped packets (`rx_out_of_buffer`) before the kernel saw them. Raising the ring to its maximum, 8192, removed most of those drops but not the port-level drops (`rx_discards_phy`). Events were still lost; see step 5b. To raise it on the receiving node:

```bash
ethtool -g enp1s0f1np1                       # "Current hardware settings: RX:" (max under "Pre-set maximums")
sudo ethtool -G enp1s0f1np1 rx 8192          # now; lost on reboot
# keep it across reboots and NetworkManager reactivation:
sudo nmcli connection modify "Wired connection 1" ethtool.ring-rx 8192
sudo nmcli device reapply enp1s0f1np1
```
Applying it briefly restarts the NIC's receive queues, which pauses traffic for about a second. SSH to the ebpf nodes goes over this same NIC. To revert: `sudo ethtool -G enp1s0f1np1 rx 1024`.
The ring keeps about 5 GB of buffers in memory (63 queues × 8192 × ~9 KB). The nodes have 503 GB.
As of 2026-10-19 both nodes are at 8192, set with `ethtool -G` only, so it's lost on reboot. Check with `ethtool -g` before throughput runs.

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
    --pool sc26 --cont-name e2sar-test --prefix $PREFIX \
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
    --pool sc26 --cont-name e2sar-test --prefix $PREFIX \
    --timeout 10000 -b 1073741824 --verbose
```
It should print `Listening on: 129.57.178.86 ports 20480:20480`.

**ebpf2201, sender:**
```bash
ip route get 129.57.178.86        # expect: dev enp1s0f1np1 src 129.57.178.85
$E2SAR_BIN/e2sar_ft -s --ip 129.57.178.85 --mtu 9000 --rate 5 -p $SRC/*
```
Ctrl-C the receiver after the last `committed` line, then verify on ebpf2203 (step 7) and clean up both nodes (step 8).

## 5b. High throughput: parallel senders to parallel receive ports

A single `e2sar_ft` sends each file (event) on one thread, so one large file tops out at about 10–12 Gbps. Several files in one `e2sar_ft` do go out in parallel, but always to one destination port. A single receive thread can't keep up, and every event is lost (see [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md#6-multi-thread-segmenter-several-files-in-one-e2sar_ft)).
To go faster, run **N `e2sar_ft` processes**, one file each, each to its own port with its own `--dataid`. On the receiver, use `--threads N`, which listens on N consecutive ports.

**ebpf2203, receiver** (N = 4 listens on ports 20480–20483):
```bash
N=4
./build/bin/e2sar_daos_recv --ip 129.57.178.86 --port $PORT --threads $N \
    --pool sc26 --cont-name e2sar-test --prefix $PREFIX \
    --timeout 10000 -b 1073741824 --deq 4 --inflight 128 --verbose
```

**ebpf2201, N senders** (stage N files in `$SRC` first):
```bash
N=4; FILES=($(ls $SRC/*.evio | head -$N)); pids=()
for i in "${!FILES[@]}"; do
  EJFAT_URI="ejfat://useless@127.0.0.1:18347/lb/1?data=129.57.178.86:$((PORT+i))&sync=129.57.178.86:19020" \
    $E2SAR_BIN/e2sar_ft -s --ip 129.57.178.85 --mtu 9000 --rate -1 --sockets 1 \
        --dataid $((4321+i)) -p "${FILES[$i]}" > $RUN/send_$i.log 2>&1 &
  pids+=($!)
done
wait "${pids[@]}"; grep -h 'goodput' $RUN/send_*.log
```
Each process restarts event numbers at 0. The distinct `--dataid` keeps their keys apart in the reassembler and in DAOS (`<PREFIX>/0/4321`, `<PREFIX>/0/4322`, ...).
Verify with step 7. The digest comparison works the same way, since `e2sar_daos_read` lists every data id under the prefix.

2–3 senders (20–26 Gbps in total) ran cleanly every time. 4 senders (about 48 Gbps) succeeded in only 1 of 6 runs, and 8 or more never did.
Raising the RX ring to 8192 and pinning the receive threads away from the NIC's interrupt cores (`--cores 64 …`) did not change this. A few dozen to a few hundred frames are still dropped at the receiving NIC port (`rx_discards_phy`), and one lost frame loses a whole ~2 GiB event.
See [BOTTLENECK_ANALYSIS.md, table 8](BOTTLENECK_ANALYSIS.md#8-parallel-senders-with-rx-ring-8192-and-pinned-receive-threads).

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

These runs read the source from `$HOME` (NFS), but the file was already in ebpf2201's memory cache. Uncached NFS reads run at only about 2.2 Gbps, which is why step 3 stages the source on `/scratch`.

### Parallel senders (step 5b), ebpf2201 → ebpf2203, source on `/scratch`

Each file is about 2.1 GiB from `~/evio-data`. "Sum of senders" adds up each `e2sar_ft`'s own goodput. Every run's DAOS data was read back and its digests checked before cleanup.

| Senders / receive threads | Sum of senders | Committed | NIC `rx_out_of_buffer` | Digests |
|---|---|---|---|---|
| 2 / 2 | 19.9 Gbps | 2 / 2 | – | PASS |
| 2 / 2 | 25.2 Gbps | 2 / 2 | 0 | PASS |
| 3 / 3 | 26.3 Gbps | 3 / 3 | 0 | PASS |
| 4 / 4 | 48.2 Gbps | 4 / 4 | 0 | PASS |
| 4 / 4 | 48.4 Gbps | 0 / 4 | not measured (drops seen in cumulative counter) | – |
| 8 / 8 | 88.1 Gbps | 0 / 8 | not measured (drops seen in cumulative counter) | – |

The runs above had the RX ring at 1024. The runs below had it at 8192, with NIC counters recorded before and after each run. The last four also pinned the receive threads to CPUs 64+.

| Senders / receive threads | Receiver option | Committed | NIC `rx_out_of_buffer` | NIC `rx_discards_phy` | Kernel `RcvbufErrors` |
|---|---|---|---|---|---|
| 4 / 4 | `--threads 4` | 0 / 4 | 132,951 | 961 | 0 |
| 4 / 4 | `--threads 4` | 0 / 4 | 0 | 11 | 0 |
| 8 / 8 | `--threads 8` | 0 / 8 | 183,193 | 340 | 0 |
| 8 / 8 | `--threads 8` | 0 / 8 | 0 | 192 | 0 |
| 12 / 12 | `--threads 12` | 0 / 12 | 0 | 231 | 181,561 |
| 16 / 16 | `--threads 16` | 0 / 16 | 0 | 1,229 | 0 |
| 4 / 4 | `--cores 64-67` | 0 / 4 | 0 | 36 | 0 |
| 4 / 4 | `--cores 64-67` | 0 / 4 | 0 | 62 | 0 |
| 8 / 8 | `--cores 64-71` | 0 / 8 | 85,900 | 240 | 0 |
| 8 / 8 | `--cores 64-71` | 0 / 8 | 0 | 426 | 0 |

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
| Above ~10 Gbps | N `e2sar_ft` processes, one port each, receiver `--threads N`, N = 2–3 | One `e2sar_ft` uses one port. From 4 on, the receiving NIC port drops a few frames in almost every run (step 5b) |
| Receiver | no `--threads` > 1 with a single `e2sar_ft` | The extra ports get no traffic |

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Receiver gets nothing. Sender exits 0 but shows `N errors` / `Connection refused` | **⚠** `EJFAT_URI` has `data=<ip>` with no port. `e2sar_ft` 0.4.0rc1 then sends to ports 16384/20480/24576/28672. Write `data=<ip>:$PORT`. To confirm, run `strace -f -e trace=connect $E2SAR_BIN/e2sar_ft ...` |
| Sender prints `0 frames sent, 0 errors` and no `Queueing file` lines, exit 0 | The source path doesn't exist on **this** node (`/scratch` is local). `e2sar_ft` reports no error for a missing path |
| Sender exits 255: `Option 'send' requires option 'path'` | A list-valued option (`-o sendmmsg`, `--cores ...`) swallowed the file names. Pass files with `-p` |
| Sender exits 255: `Conflicting options 'cores' and 'sockets'` | With `--cores`, the number of send threads is the number of cores. Drop `--sockets` |
| Large file missing, `Lost in reassembly: 1`, `RcvbufErrors` 0 | The event wasn't complete within `--timeout`. Use `--timeout 10000`, `--mtu 9000`, and no `--smooth` |
| Large file missing, `RcvbufErrors` > 0 | The receive socket overflowed. Use `-b 1073741824` |
| Several files in one `e2sar_ft`: every event lost, `RcvbufErrors` in the hundreds of thousands | The sender sends the files in parallel (43–70 Gbps), but all to one port, so one receive thread gets everything. Use parallel senders (step 5b) |
| Events lost, `RcvbufErrors` 0 | Dropped in the NIC before the kernel. Check `ethtool -S enp1s0f1np1 \| grep -E 'rx_out_of_buffer\|rx_discards_phy'` before and after. `rx_out_of_buffer`: raise the RX ring to 8192 (see [NIC RX ring](#nic-rx-ring-receiver-needs-root)), and check whether two flows share a queue (`ethtool -S \| grep rxN_packets`). `rx_discards_phy`: the NIC port's buffer overflowed. The switch doesn't pass the receiver's pause frames back to the sender, so use fewer parallel senders |
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
