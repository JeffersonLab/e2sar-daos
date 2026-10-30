---
type: How-To
title: "Testing through the ESnet EJFAT load balancer"
description: "Send files with e2sar_ft from ebpf2201 through the ESnet EJFAT LB to e2sar_daos_recv on ebpf2203, verify the restored files, and clean up."
tags: [lb, ejfat, e2sar_ft, ebpf-testbed]
stale_after: 2026-10-29T23:15:16Z
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/ESNET_LB_TESTING.md", title: "ESNET_LB_TESTING.md at 0d85d00" }
---

# Testing through the ESnet EJFAT load balancer

> **Stale since layout v2 ([#31](https://github.com/JeffersonLab/e2sar-daos/pull/31)):** the commands below still use `--prefix`, `--prefixes` or `--oid`, which `e2sar_daos_recv` and `e2sar_daos_read` no longer have. A run is now one container (`--cont-name` / `--cont`). Update and rerun before relying on them ([#26](https://github.com/JeffersonLab/e2sar-daos/issues/26)).

This is the full procedure for sending files with `e2sar_ft` from **ebpf2201**, through the ESnet EJFAT load balancer (LB), to `e2sar_daos_recv` on **ebpf2203**.
The receiver writes each event into DAOS, and `e2sar_daos_read` restores the files and checks them against the source digests.
Every command runs on the nodes themselves. Open one terminal on each node, and start `bash` in each, since the login shell is tcsh.
Last run on 2026-10-19 against `feature/daos-kv-recv-apps`, using LB instance 638 on `ejfat-lb.es.net`. See [Results](../results/2026-10-19-lb-e2sar-ft.md#results-from-2026-10-19).

**Use the `stable4` LB from now on (ESnet's recommendation, 2026-10-26):** control plane `192.188.29.6:18048`, data plane `192.188.29.54`. The commands below use it. The results (LB 638 and 640) were measured on the older control plane `ejfat-lb.es.net:18008`, data plane `192.188.29.16`.

```
ebpf2201 (129.57.178.85)          ESnet LB                              ebpf2203 (129.57.178.86)
/scratch/<user>/e2sar_src_lb                                            DAOS sc26/e2sar-lb ──▶ /scratch/<user>/e2sar_out_lb
e2sar_ft -s --withcp ──UDP──▶ data 192.188.29.54:<ports> ──UDP──▶ e2sar_daos_recv --withcp :20600[..+N-1]
          ├── sync UDP ─────▶ sync=<ip:port> from the instance URI
          └── gRPC ─────────▶ stable4 192.188.29.6:18048 ◀──── gRPC ──── (register, state, deregister)
              (add/remove sender)
```

| Step | Node | What |
|---|---|---|
| [1](#1-environment-bashrc-and-the-lb-instance-once) | either (shared `$HOME`) | Put the E2SAR environment and admin URI in `~/.bashrc`, reserve an LB instance and source it |
| [2](#2-build-once) | either | Build e2sar-daos |
| [3](../reference/lbadm.md#3-the-lb-with-lbadm-reservation-senders-workers) | either | Reservation details, inspect the LB, and add or remove senders and workers |
| [4](#4-pre-flight-checks-before-each-run) | both | Check the LB path and state, and that nobody else is using the nodes |
| [5](#5-stage-source-files-ebpf2201) | ebpf2201 | Copy the source files to `/scratch` and save digests |
| [6](#6-start-the-receiver-ebpf2203) | ebpf2203 | Start the receiver; it registers with the LB |
| [7](#7-send-ebpf2201) | ebpf2201 | Send through the LB |
| [8](#8-stop-the-receiver-ebpf2203) | ebpf2203 | Stop the receiver; it deregisters |
| [9](#9-verify-and-restore-ebpf2203) | ebpf2203 | Restore from DAOS and compare digests |
| [10](#10-clean-up-required-after-every-run) | both | Delete source copies, restored files and DAOS data |
| [11](#11-highest-lossless-throughput) | both | Optional: push throughput with several files and receive ports |

Node facts:

| | ebpf2201 (sender) | ebpf2203 (receiver) |
|---|---|---|
| Data-plane NIC `enp1s0f1np1`, 100 Gb/s, MTU 9000, RX ring 8192 | 129.57.178.85 | 129.57.178.86 |
| `/scratch` (local XFS) | source copies | restored files |
| DAOS 2.6.5 client, pool `sc26` | yes | yes |

- `$HOME` (`/testbed/home/<user>`) is NFS, shared by both nodes, so `~/.bashrc`, the build and `~/e2sar_run` are the same on both. `/scratch/<user>` is local to each node.
- The nodes can't SSH to each other without a password. Run each step in the terminal for its node.
- E2SAR 0.4.0rc1 is staged in `$HOME/opt/e2sar`, not installed system-wide, so it isn't on `PATH`. Call the tools as `$E2SAR_BIN/...`.
- Other people run tests on these nodes. Before every run, check that both nodes are idle (step 4).
## 1. Environment: `~/.bashrc` and the LB instance (once)

`$HOME` is shared, so one `~/.bashrc` serves both nodes. Add this block, with your own tokens, and keep the file private (`chmod 600 ~/.bashrc`, home directory mode 700).
**Never copy the tokens into git.** To install E2SAR itself, see [install-e2sar](install-e2sar.md).

```bash
## E2SAR
export E2SAR_ROOT=$HOME/opt/e2sar           # E2SAR staging root
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH
export E2SAR_BIN=$E2SAR_ROOT/usr/local/bin   # not on PATH; call the tools as $E2SAR_BIN/<tool>

## ESnet EJFAT LB (secrets: keep this file private, never copy into git)
export EJFAT_ADMIN_URI='ejfats://<admin token>@192.188.29.6:18048/lb/0'   # stable4; admin token: reserve/free/overview only
```
`meson` and `ninja` must be on `PATH` too (`export PATH=$HOME/.local/bin:$PATH`).

Next, reserve an LB instance with the admin URI. `-e` prints only `export EJFAT_URI='<instance URI>'`, so the output can go straight into a file that you source:

```bash
bash                                       # from tcsh; this reads ~/.bashrc
ls -l ~/e2sar-instance.env 2>/dev/null     # must not exist: '>' would overwrite the only copy of an older instance token
(umask 077; $E2SAR_BIN/lbadm --reserve -v -u "$EJFAT_ADMIN_URI" -l <name> -a 129.57.178.85 -d 24:00:00 -e > ~/e2sar-instance.env)
cut -d@ -f2 ~/e2sar-instance.env           # expect: 192.188.29.6:18048/lb/<id>?sync=...&data=192.188.29.54:...'
source ~/e2sar-instance.env
$E2SAR_BIN/lbadm --status                  # the new LB ID, with 129.57.178.85 under Registered senders
```
- `-u "$EJFAT_ADMIN_URI"`: reserving needs the admin token. The instance URI can't reserve.
- `-l <name>` is a name of your choice (it shows up in `--overview`). `-d 24:00:00` reserves it for 24 hours. `-a` puts ebpf2201 on the sender allowlist at once. The options are explained in 3.3.
- `-v` (`--novalidate`) skips the TLS certificate check, for when the CA isn't installed. If you need it here, add it to every other `lbadm` call too.
- If the file is empty or holds an error message, the reservation failed. Delete the file, fix the cause, and try again.
- `umask 077` makes the file readable only by you. It holds the instance token, which the LB never shows again: don't lose it, and never copy it into git.

Do this once per reservation, on either node, since `$HOME` is shared.

Then, in **each** terminal, source the instance and set the per-test variables. The port, container and directories differ from the node-to-node tests, which use 20480 and `e2sar-test`, so the two can't collide:

```bash
bash                                   # from tcsh; this reads ~/.bashrc
source ~/e2sar-instance.env            # EJFAT_URI: the reserved instance
export PORT=20600                      # first receiver UDP port; the worker registers it with the LB
export CONT=e2sar-lb                   # DAOS container used only for LB tests
export SCR=/scratch/$USER
export SRC=$SCR/e2sar_src_lb           # sender input (ebpf2201)
export OUT=$SCR/e2sar_out_lb           # receiver output (ebpf2203)
export RUN=$HOME/e2sar_run             # shared: digests and logs
mkdir -p $RUN
```

`EJFAT_URI` is now the LB instance URI in this shell. Check it before using the LB. The command prints only the part after the token:
```bash
echo "$EJFAT_URI" | cut -d@ -f2        # expect: 192.188.29.6:18048/lb/<id>?sync=...&data=192.188.29.54:...
```
The node-to-node procedure exports a placeholder, `ejfat://useless@10.10.10.10:1234/lb/1?data=...`, which overrides it for that shell. With the placeholder set, `lbadm --status` fails with `failed to connect ... 10.10.10.10:1234`. Use a separate shell for LB tests, or run `source ~/e2sar-instance.env` again.
Scripts run as `bash -s` or `bash script.sh` don't read `~/.bashrc`. Start them with `source ~/.bashrc; source ~/e2sar-instance.env`.

## 2. Build (once)

```bash
cd ~/daq/e2sar-daos

meson setup build                          # --wipe after an E2SAR upgrade
meson compile -C build
meson test -C build --suite unit           # Ok: 3, Fail: 0
./build/bin/e2sar_daos_recv --help | grep withcp     # -c [ --withcp ] enable control plane interactions
```

## 4. Pre-flight checks (before each run)

**LB path** (on each node, once per session):
```bash
ip route get 192.188.29.54                        # expect: dev enp1s0f1np1 src 129.57.178.8x
ping -c2 -M do -s 8972 192.188.29.54              # MTU 9000 works (RTT was ~71 ms to the older data plane)
timeout 5 bash -c '</dev/tcp/192.188.29.6/18048' && echo CP-OK
```

**LB state:**
```bash
$E2SAR_BIN/lbadm --status
```
Expected: `LB ID`, `LB details: expiresat=...`, `Registered senders: ...` and `Registered workers:`.
Write down the sender list so you can check at the end that you left it as it was. LB 638 already lists other JLab and NERSC senders.
The worker list should be empty. If it isn't, someone else is receiving on this LB, and events would be split between their workers and yours.
Any other allowlisted sender that sends while you are the only worker sends to *your* receiver. On 2026-10-19 this happened: an allowlisted sender at 129.57.69.58 was sending ~15k packets/s of ~100-byte packets with a version-2 RE header through LB 638. E2SAR 0.4.0rc1 discards them as `Bad RE Header Discards`, so they don't corrupt your events, but they inflate `rx_packets_phy` by ~0.5 M per run.

**Nodes idle** (on both nodes):
```bash
ps -eo user,pid,etime,args | grep -E 'e2sar_(ft|daos_recv)' | grep -v grep     # expect nothing
ss -uanp | grep -E ':20(4[89]|6[0-3])[0-9]\b'                                # nothing on 20480-20499 or 20600-20639
daos cont list sc26                                                           # e2sar-lb should not exist yet
ethtool -g enp1s0f1np1 | sed -n '/Current/,$p' | grep '^RX:'                    # RX: 8192
```
Concurrent tests share both NICs, the CPUs and DAOS, and they spoil each other's numbers and losses. If something is running, wait or ask its owner. Don't kill someone else's run.

## 5. Stage source files (ebpf2201)

For the basic test, use one 1.78 GB EVIO file and one random file whose size is not a multiple of 1 MiB. Generate the random file once in `$RUN` and reuse it, so the digests stay the same.

```bash
mkdir -p $SRC
cp $HOME/evio-data/frames_thread0_file0020.evio $SRC/
[ -f $RUN/random_3500K.bin ] || head -c 3500K /dev/urandom > $RUN/random_3500K.bin
cp $RUN/random_3500K.bin $SRC/
(cd $SRC && sha256sum -- *) | tee $RUN/src_lb.sha256
```

## 6. Start the receiver (ebpf2203)

Use a new `PREFIX` for every send. `e2sar_ft` numbers events from 0 each time, so a second send under the same prefix overwrites the first in DAOS.
Use the same `PREFIX` value on both nodes. It is only used on ebpf2203, but naming the logs after it helps.

```bash
export PREFIX=lb_$(date +%s)
nstat -n                                          # reset the UDP counter baseline for this run
ethtool -S enp1s0f1np1 | grep -E ' (rx_packets_phy|rx_out_of_buffer|rx_discards_phy):' > $RUN/$PREFIX.nic0
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_recv --withcp --ip 129.57.178.86 --port $PORT \
    --pool sc26 --cont-name $CONT --prefix $PREFIX \
    --timeout 10000 -b 1073741824 --verbose 2>&1 | tee $RUN/$PREFIX.recv.log
```

Expected startup output:
```
DAOS pool/container:  sc26/e2sar-lb (created)
Control plane:        ON
Listening on:         129.57.178.86 ports 20600:20600
Receiving; Ctrl-C to stop
```

With `--withcp`, the receiver registers as a worker under its hostname, with `--ip`, `--port` and its number of ports, then sends state updates to the LB.
Wait about 10 s, then check from a second ebpf2203 terminal that the LB has given it slots:

```bash
$E2SAR_BIN/lbadm --status | grep slotsassigned
# [ name=ebpf2203.jlab.org, ..., slotsassigned=512, ..., IP Address=129.57.178.86, UDP Port=20600, ... ]
```
Don't send until `slotsassigned` is above 0.

## 7. Send (ebpf2201)

```bash
$E2SAR_BIN/e2sar_ft -s --withcp --ip 129.57.178.85 --mtu 9000 --rate 5 -p $SRC/* 2>&1 | tee $RUN/lb_send.log
```

Expected output:
```
Adding senders to LB: 129.57.178.85 ... done
Control plane:                 ON
Queueing file "/scratch/<user>/e2sar_src_lb/frames_thread0_file0020.evio"  as event 0
Queueing file "/scratch/<user>/e2sar_src_lb/random_3500K.bin"  as event 1
Estimated goodput (Gbps): 7.12
Completed, 199184 frames sent, 0 errors
Removing senders: 129.57.178.85
```

- With `--withcp`, `e2sar_ft` adds its own `--ip` to the LB sender allowlist and removes it at exit, so you don't need `lbadm --addsenders`.
  `--ip` must be the address the packets actually leave from (see `ip route get` in step 4), or the LB drops them.
- Running two `e2sar_ft --withcp` processes from the same IP at the same time is unsafe. The first one to exit removes the shared IP from the allowlist while the other is still sending. For more throughput, use one process with several files (step 11).
- The receiver prints one `committed <PREFIX>/<evt>/4321 (<bytes> bytes)` line per file.

To try the path first, send the small file alone (`-p $SRC/random_3500K.bin`), then use a new `PREFIX` for the full run.

## 8. Stop the receiver (ebpf2203)

Press Ctrl-C after the last `committed` line. On exit the receiver deregisters from the LB and prints its statistics:

```
	Lost in reassembly:        0
	Lost in enqueue:           0
	Events committed:          2
	Events failed:             0
	Ingest throughput:         9.296 Gbps
	DAOS commit throughput:    8.531 Gbps
```

Check that it left cleanly and that nothing was dropped:
```bash
$E2SAR_BIN/lbadm --status | tail -2               # "Registered workers:" with nothing after it
nstat -z UdpRcvbufErrors                          # kernel socket drops since step 6: 0
ethtool -S enp1s0f1np1 | grep -E ' (rx_packets_phy|rx_out_of_buffer|rx_discards_phy):' > $RUN/$PREFIX.nic1
paste $RUN/$PREFIX.nic0 $RUN/$PREFIX.nic1 | awk '{print $1, "+" $4-$2}'   # rx_out_of_buffer and rx_discards_phy: +0
```
For what the throughput numbers mean, and their caveats, see [Throughput metrics](../concepts/throughput-metrics.md#throughput-metrics).

## 9. Verify and restore (ebpf2203)

```bash
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_read --cont $CONT --prefix $PREFIX            # one row per file
mkdir -p $OUT/$PREFIX
./build/bin/e2sar_daos_read --cont $CONT --prefix $PREFIX --fetch -o $OUT/$PREFIX | tee $OUT/$PREFIX.sha256
diff <(cut -d' ' -f1 $RUN/src_lb.sha256 | sort) \
     <(grep -E '^[0-9a-f]{64} ' $OUT/$PREFIX.sha256 | cut -d' ' -f1 | sort) && echo PASS
```

Expected output:
```
a2da2604...6617  lb_<ts>_0_4321.evio  (1776313996 bytes, 1695 chunks, EVIO v6 big-endian)
51152dfc...403a  lb_<ts>_1_4321.evio  (3584000 bytes, 4 chunks, not EVIO)
2 events verified, 0 failed
PASS
```
`$RUN/src_lb.sha256` was written on ebpf2201, but `$RUN` is in the shared `$HOME`, so it's readable here.
Restored files are named `<PREFIX>_<evt>_4321.evio`. To view them, see the viewer part of [ebpf-testbed §4](../sites/ebpf-testbed.md#4-applications-end-to-end-on-ebpf2203).

## 10. Clean up (required, after every run)

Each run leaves the source copy on the sender, the restored files on the receiver and the event data in DAOS. Remove all three once the run is verified.
Keep `$RUN`, which holds the logs, digests and `random_3500K.bin`.

```bash
# ebpf2203: restored files and the LB test container
rm -rf $OUT
daos cont destroy sc26 $CONT                      # e2sar_daos_recv recreates it on the next run
ls /scratch/$USER; daos cont list sc26            # no e2sar_out_lb, no e2sar-lb
# ebpf2201: source copies
rm -rf $SRC
ls /scratch/$USER                                 # no e2sar_src_lb
# either node: no workers, and the same senders as in step 4
$E2SAR_BIN/lbadm --status
```

Delete only what you created. Leave other users' directories and containers alone, and other senders on the LB too.

## 11. Highest lossless throughput

One event goes out on one sender thread at about 10 Gbps, so one large file can't go faster. To go faster, queue several files in **one** `e2sar_ft`. The multi-thread Segmenter sends them in parallel, one thread per event.
Through the LB this works where node-to-node didn't. E2SAR gives each event a random 16-bit entropy value, and the LB uses it to pick one of the worker's ports. With `--threads T`, the receiver opens and registers T ports, so the events land on different receive threads.
Without an LB, every event goes to one port and one thread, and all of them were lost (see [2026-10-19-bottleneck-e2sar-ft, table 6](../results/2026-10-19-bottleneck-e2sar-ft.md#6-multi-thread-segmenter-several-files-in-one-e2sar_ft)).

**ebpf2201: stage N files of about 2.1 GiB each:**
```bash
N=2                                              # 2: reliable; 3: the ceiling, about half the runs lossless
mkdir -p $SRC
for i in $(seq 0 $((N-1))); do cp $HOME/evio-data/frames_thread0_file000$i.evio $SRC/ & done; wait
(cd $SRC && sha256sum -- *.evio) | tee $RUN/src_lb.sha256
```

**ebpf2203: the receiver with T ports** (T a power of 2, much larger than N):
```bash
export PREFIX=lbhi_$(date +%s); T=16
nstat -n; ethtool -S enp1s0f1np1 | grep -E ' (rx_packets_phy|rx_out_of_buffer|rx_discards_phy):' > $RUN/$PREFIX.nic0
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_recv --withcp --ip 129.57.178.86 --port $PORT --threads $T \
    --pool sc26 --cont-name $CONT --prefix $PREFIX \
    --timeout 10000 -b 1073741824 --deq 4 --inflight 128 --timing-csv $RUN/$PREFIX.csv 2>&1 | tee $RUN/$PREFIX.recv.log
```
It should print `Listening on: 129.57.178.86 ports 20600:20615`. Wait for `slotsassigned` > 0 as in step 6.

**ebpf2201: one sender, N threads, no rate limit:**
```bash
$E2SAR_BIN/e2sar_ft -s --withcp --ip 129.57.178.85 --mtu 9000 --rate -1 --sockets $N -p $SRC/*.evio 2>&1 | tee $RUN/lbhi_send.log
```
`--rate -1` matters. Any rate limit adds a gap between events, and the threads then send one after another.

Stop, check, verify and clean up as in steps 8–10. While the receiver is still running, `ss -uanm | grep -A1 ':206'` shows per-port socket drops (`d<N>` in `skmem`).
A run is lossless only if all N digests match. Check the NIC counters too: any increase in `rx_discards_phy` means a lost event.

## Recommended settings

| | Setting | Why |
|---|---|---|
| Both | `--withcp` | Without it, the receiver doesn't register, so the LB has nowhere to send |
| Both | Instance URI with the `data=` range as given | The LB, not the URI, chooses the worker port |
| Sender | `--ip <address the packets leave from>` | It goes on the LB sender allowlist |
| Sender | `--mtu 9000` | The LB path supports it, and at 1500 the sender is limited to ~2 Gbps |
| Sender | One `e2sar_ft` process per sender IP | Each `--withcp` process removes the shared IP from the allowlist when it exits |
| Sender | Basic: `--rate 5`. Throughput: `--rate -1 --sockets N` with N ≤ 3 files | A rate limit serializes events. Above ~25 Gbps the receiving NIC port discards frames |
| Sender | No `--smooth` | It caps the sender below 1 Gbps, too slow for a multi-GB event in 10 s |
| Sender | `-p $SRC/*` | Required after list-valued options, which otherwise swallow the file names |
| Receiver | `--threads 16` (or 32) for N > 1 | More ports make it less likely that two events share a port |
| Receiver | `-b 1073741824` | Avoids kernel socket drops (`RcvbufErrors`) |
| Receiver | `--timeout 10000` | The maximum. The default 500 ms drops multi-GB events |
| Receiver | `--deq 4 --inflight 128` for N > 1 | Keeps DAOS ahead of several events arriving at once |
| Receiver | New `--prefix` per send | Event numbers restart at 0 |

## Troubleshooting

Rows marked † happened in these runs. The others are what to check first, based on how the LB and E2SAR work.

| Symptom | Cause and fix |
|---|---|
| † Receiver: `Unable to open receive sockets: Address already in use` | Another receiver holds `$PORT`. Check with `ss -uanp \| grep :$PORT`, and pick another port rather than killing someone else's run |
| † Several events lost, 0 socket drops, `rx_discards_phy` grew | Total rate above ~25 Gbps. The receiving NIC port dropped frames. Send fewer files at once |
| † Events lost, one port shows large socket drops (`ss -uanm`) | Two events picked the same port. Use more receiver `--threads` (16–32) |
| † An event missing from `Events committed`, but `Lost in reassembly` is 0 | None of its packets arrived at all. Check whether another test was using the nodes or the LB at the same time |
| † NIC counters grew far more than the frames sent | Other traffic is reaching the NIC. Either another user's test is running on the node, or another allowlisted sender on the LB is sending: LB 638 carried a ~15k pps stream with a v2 RE header from an external sender, which the Reassembler discards as bad headers. Compare `rx_discards_phy`/`rx_out_of_buffer`, not `rx_packets_phy` |
| Receiver runs, but `lbadm --status` lists no worker | It was started without `--withcp`, or it exited. Read its output |
| Worker listed with `slotsassigned=0` | The LB hasn't recomputed the schedule yet. Wait a few seconds before sending |
| Sender shows `0 errors`, but the receiver gets nothing | The sender's `--ip` isn't the address the packets leave from, so the LB drops them. Check `ip route get 192.188.29.54`. Also check that inbound UDP to the receiver ports isn't firewalled |
| `lbadm --status`: `failed to connect to all addresses ... 10.10.10.10:1234` | The shell still has the node-to-node placeholder `EJFAT_URI`. Run `source ~/e2sar-instance.env` |
| `lbadm --overview`: `Error in parsing URI from command-line` | `$EJFAT_ADMIN_URI` is empty in this shell. Run `source ~/.bashrc` |
| A worker stays in `--status` after its receiver was killed | Wait about 10 s for the LB to drop it, or `lbadm --deregister -s <SID>` if you have the session ID (3.5) |
| `e2sar_ft` or `lbadm` can't find libraries, or `EJFAT_URI` is empty | The shell didn't read `~/.bashrc` or the instance file: tcsh, or a `bash -s` script. Run `bash`, then `source ~/.bashrc; source ~/e2sar-instance.env` |
| Large file missing, `Lost in reassembly: 1`, no drops anywhere | The event wasn't complete within `--timeout`. Use `--timeout 10000`, `--mtu 9000` and no `--smooth` |
| gRPC or certificate errors | The URI must be `ejfats://`. If the certificate doesn't match the stable4 IP `192.188.29.6`, or the CA isn't installed, add `-v` (`--novalidate`) to `lbadm`, `e2sar_ft` and `e2sar_daos_recv` |
| Either app exits: `NUMA management not available` | This E2SAR build has no NUMA support. Drop `--numa` |
