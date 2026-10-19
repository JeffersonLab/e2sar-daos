# Testing through the ESnet EJFAT load balancer

This is the full procedure for sending files with `e2sar_ft` from **ebpf2201**, through the ESnet EJFAT load balancer (LB), to `e2sar_daos_recv` on **ebpf2203**.
The receiver writes each event into DAOS, and `e2sar_daos_read` restores the files and checks them against the source digests.
Every command runs on the nodes themselves. Open one terminal on each node, and start `bash` in each, since the login shell is tcsh.
Last run on 2026-10-19 against `feature/daos-kv-recv-apps`, using LB instance 638 on `ejfat-lb.es.net`. See [Results](#results-from-2026-10-19).

```
ebpf2201 (129.57.178.85)          ESnet LB                              ebpf2203 (129.57.178.86)
/scratch/<user>/e2sar_src_lb                                            DAOS sc26/e2sar-lb ──▶ /scratch/<user>/e2sar_out_lb
e2sar_ft -s --withcp ──UDP──▶ data 192.188.29.16:16384-32767 ──UDP──▶ e2sar_daos_recv --withcp :20600[..+N-1]
          ├── sync UDP ─────▶ sync 192.188.29.6:19016
          └── gRPC ─────────▶ ejfat-lb.es.net:18008 ◀──────── gRPC ──── (register, state, deregister)
              (add/remove sender)
```

| Step | Node | What |
|---|---|---|
| [1](#1-environment-bashrc-once) | either (shared `$HOME`) | Put the E2SAR environment and LB URIs in `~/.bashrc` |
| [2](#2-build-once) | either | Build e2sar-daos |
| [3](#3-lb-reservation-and-uri) | either | Get an LB instance URI, or reserve one |
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
- ebpf2203 also has an e2sar 0.3.1 RPM in `/usr/local/bin`. Always call the 0.4.0rc1 tools as `$E2SAR_BIN/...`.
- Other people run tests on these nodes. Before every run, check that both nodes are idle (step 4).

## 1. Environment: `~/.bashrc` (once)

`$HOME` is shared, so one `~/.bashrc` serves both nodes. Add this block, with your own tokens, and keep the file private (`chmod 600 ~/.bashrc`, home directory mode 700).
**Never copy the tokens into git.** To install E2SAR itself, see [E2SAR_INSTALL.md](E2SAR_INSTALL.md).

```bash
## E2SAR
export E2SAR_ROOT=$HOME/opt/e2sar           # E2SAR staging root
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH
export E2SAR_BIN=$E2SAR_ROOT/usr/local/bin   # call tools by full path; ebpf2203 also has e2sar 0.3.1 in /usr/local/bin

## ESnet EJFAT LB (secrets: keep this file private, never copy into git)
export LBTOKEN=<instance token>                                       # instance token for LB <id>
export EJFAT_URI='ejfats://'${LBTOKEN}'@ejfat-lb.es.net:18008/lb/<id>?sync=192.188.29.6:19016&sync=[2001:400:a300::6]:19016&data=192.188.29.16:16384-32767&data=[2001:400:a300::16]:16384-32767'
export EJFAT_ADMIN_URI='ejfats://<admin token>@ejfat-lb.es.net:18008/lb/0'   # admin token: reserve/free/overview only
```
`meson` and `ninja` must be on `PATH` too (`export PATH=$HOME/.local/bin:$PATH`).

Then, in **each** terminal, set the per-test variables. The port, container and directories differ from the back-to-back procedure in [EBPF_TESTBED_NOTES.md](EBPF_TESTBED_NOTES.md), which uses 20480 and `e2sar-test`, so the two can't collide:

```bash
bash                                   # from tcsh; this reads ~/.bashrc
export PORT=20600                      # first receiver UDP port; the worker registers it with the LB
export CONT=e2sar-lb                   # DAOS container used only for LB tests
export SCR=/scratch/$USER
export SRC=$SCR/e2sar_src_lb           # sender input (ebpf2201)
export OUT=$SCR/e2sar_out_lb           # receiver output (ebpf2203)
export RUN=$HOME/e2sar_run             # shared: digests and logs
mkdir -p $RUN
```

`EJFAT_URI` is now the LB instance URI in every bash shell. The back-to-back procedure sets its own `EJFAT_URI`, which overrides it for that shell.
Scripts run as `bash -s` or `bash script.sh` don't read `~/.bashrc`. Start them with `source ~/.bashrc`.

## 2. Build (once)

```bash
cd ~/daq/e2sar-daos
git pull https://github.com/JeffersonLab/e2sar-daos.git feature/daos-kv-recv-apps   # SSH to GitHub is blocked
meson setup build                          # --wipe after an E2SAR upgrade
meson compile -C build
meson test -C build --suite unit           # Ok: 3, Fail: 0
./build/bin/e2sar_daos_recv --help | grep withcp     # -c [ --withcp ] enable control plane interactions
```

## 3. LB reservation and URI

There are two kinds of URI. Both live only in `~/.bashrc`.

- **Admin URI** (`$EJFAT_ADMIN_URI`), `ejfats://<admin token>@ejfat-lb.es.net:18008/lb/0`. Only for `lbadm --reserve`, `--free`, `--overview` and token management. The apps don't need it.
- **Instance URI** (`$EJFAT_URI`), which identifies one reserved LB. It carries the instance token, the LB id, the sync addresses and the data address range.

The apps and `lbadm --status` read `$EJFAT_URI`.
In the back-to-back case the URI needs `data=<ip>:<port>`, but here keep the `data=` range exactly as the LB gives it. The LB picks the worker port from what the receiver registers.

**If you already have an instance** (LB 638 was already reserved for these tests), check it and go to step 4:
```bash
$E2SAR_BIN/lbadm --status | head -4        # LB ID: 638, LB details: expiresat=...
```

**To reserve a new instance** (not run in these tests), use the admin URI. `-a` adds sender IPs to the allowlist, `-d` sets the duration, and `-e` prints the `export EJFAT_URI=...` line for the new instance, which goes into `~/.bashrc`:
```bash
$E2SAR_BIN/lbadm -u "$EJFAT_ADMIN_URI" --reserve -l <name> -a 129.57.178.85 -d 24:00:00 -e
```
When you are done with your own reservation, free it with `lbadm --free`, using the instance URI.
Never free an instance that someone else reserved.

## 4. Pre-flight checks (before each run)

**LB path** (on each node, once per session):
```bash
ip route get 192.188.29.16                        # expect: dev enp1s0f1np1 src 129.57.178.8x
ping -c2 -M do -s 8972 192.188.29.16              # MTU 9000 works; RTT ~71 ms
timeout 5 bash -c '</dev/tcp/ejfat-lb.es.net/18008' && echo CP-OK
```

**LB state:**
```bash
$E2SAR_BIN/lbadm --status
```
Expected: `LB ID`, `LB details: expiresat=...`, `Registered senders: ...` and `Registered workers:`.
Write down the sender list so you can check at the end that you left it as it was. LB 638 already lists other JLab and NERSC senders.
The worker list should be empty. If it isn't, someone else is receiving on this LB, and events would be split between their workers and yours.
Any other allowlisted sender that sends while you are the only worker sends to *your* receiver. On 2026-10-19 this happened: an allowlisted sender, probably the coda-fb sender at 129.57.69.58, was sending ~15k packets/s of ~100-byte packets with a version-2 RE header through LB 638. E2SAR 0.4.0rc1 discards them as `Bad RE Header Discards`, so they don't corrupt your events, but they inflate `rx_packets_phy` by ~0.5 M per run.

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
    --pool sc26 --cont $CONT --prefix $PREFIX \
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
For what the throughput numbers mean, and their caveats, see [Throughput metrics](BOTTLENECK_ANALYSIS.md#throughput-metrics).

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
Restored files are named `<PREFIX>_<evt>_4321.evio`. To view them, see the viewer part of [TESTING.md §4](TESTING.md#4-applications-end-to-end-on-ebpf2203).

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
Through the LB this works where back-to-back didn't. E2SAR gives each event a random 16-bit entropy value, and the LB uses it to pick one of the worker's ports. With `--threads T`, the receiver opens and registers T ports, so the events land on different receive threads.
Without an LB, every event goes to one port and one thread, and all of them were lost (see [BOTTLENECK_ANALYSIS.md, table 6](BOTTLENECK_ANALYSIS.md#6-multi-thread-segmenter-several-files-in-one-e2sar_ft)).

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
    --pool sc26 --cont $CONT --prefix $PREFIX \
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

### Why the ceiling is about 25 Gbps

Two limits outside e2sar-daos, both seen in the sweep below:
- **NIC port discards on ebpf2203.** Above about 25 Gbps total, the receiving NIC drops a few hundred frames at the port (`rx_discards_phy`), with 0 kernel or socket drops. The switch doesn't pass ebpf2203's pause frames back toward the sender, so those frames are simply lost. Any lost frame loses its whole multi-GB event.
- **Port collisions.** Entropy is random per event, and `e2sar_ft` can't set it. With N events and T ports, two events share a port with probability 1 − ∏(1 − k/T) for k = 1…N−1, which is 18% for N = 3, T = 16, 9% for N = 3, T = 32, and 33% for N = 4, T = 16. One receive thread then takes about 20 Gbps and overflows its socket. More ports make this less likely. Use T = 16 or 32.

DAOS was not a limit. `Lost in enqueue` was 0 in every run, and 3 events (6.4 GB) committed within 0.17 s of the last one arriving.

## Results from 2026-10-19

LB 638, ebpf2201 → ebpf2203, MTU 9000, `-b 1 GiB`, `--timeout 10000`. The path to the LB had a 71 ms RTT and passed 9000-byte packets unfragmented.
For the definitions of sender goodput, ingest, DAOS commit and end-to-end (E2E), see [Throughput metrics](BOTTLENECK_ANALYSIS.md#throughput-metrics).

### Basic runs (steps 5–9, `--rate 5`, 1 receive thread)

| Run | Files | Sender goodput | Receive window | Ingest | DAOS commit | Result |
|---|---|---|---|---|---|---|
| 1 | 3.5 MB random only | (one small event) | | | 1 of 1 committed | PASS |
| 2 | 1.78 GB EVIO + 3.5 MB random | 7.12 Gbps | 1.53 s | 9.30 Gbps | 8.53 Gbps | PASS |
| 3 | same | 7.12 Gbps | 1.47 s | 9.70 Gbps | 9.01 Gbps | PASS |

Every run had 0 lost events, 0 `UdpRcvbufErrors` and matching digests. The receiver got all 512 slots within about 10 s and deregistered on Ctrl-C.
That's the same as back-to-back ebpf2201 → ebpf2203 (7.3–7.4 Gbps sent, 8.8–8.9 Gbps ingest), so the LB and the WAN path don't lower single-sender throughput.

### Throughput sweep (step 11, `--rate -1 --sockets N`, `--deq 4 --inflight 128`)

Files are about 2.1 GiB EVIO each (`frames_thread0_file000*.evio`). Both nodes were otherwise idle, and both RX rings were at 8192.
E2E is the total bytes × 8 / (sender process start → last DAOS commit), measured with chrony-synced clocks. It includes about 1–2 s of `e2sar_ft` startup and LB registration, so it understates the transfer rate for runs this short.
Ingest and DAOS commit aren't shown: with 1–4 events arriving together, their windows are too short to mean anything (see the metrics caveats).

| Files N | Ports T | Sender goodput | E2E | Committed | Socket drops | NIC `rx_discards_phy` | Result |
|---|---|---|---|---|---|---|---|
| 1 | 1 | 8.59 Gbps | 4.68 Gbps | 1 / 1 | 0 | 0 | PASS |
| 2 | 4 | 11.45 Gbps | – | 2 / 2 | 0 | 0 | PASS |
| 2 | 4 | 11.45 Gbps | – | 2 / 2 | 0 | 0 | PASS |
| 2 | 8 | 11.45 Gbps | 7.06 Gbps | 2 / 2 | 0 | 0 | PASS |
| 3 | 16 | **25.77 Gbps** | **10.42 Gbps** | 3 / 3 | 0 | 0 | **PASS** |
| 3 | 16 | 25.77 Gbps | – | 0 / 3 | 0 | 654 | FAIL |
| 3 | 32 | **25.77 Gbps** | **10.72 Gbps** | 3 / 3 | 0 | 0 | **PASS** |
| 3 | 32 | 25.78 Gbps | – | 0 / 3 | 0 | 338 | FAIL |
| 4 | 8 | 22.91 Gbps | – | 1 / 4 | 243,713 on one port | 1 | FAIL (port collision) |
| 4 | 16 | 34.36 Gbps | – | 0 / 4 | 0 | 116 | FAIL |
| 4 | 32 | 34.36 Gbps | – | 2 / 4 | 0 | 2 | FAIL |

**Highest lossless throughput through the LB: 3 concurrent 2.1 GiB events at 25.8 Gbps sender goodput, 6.4 GB in about 4.8 s end to end, lossless in 2 of 4 runs.**
**For reliable lossless runs, use 2 events at a time (11.5 Gbps, 3 of 3).** Every failure at N ≥ 3 came with NIC port discards or a port collision. None were DAOS or sender errors.
To go higher, the receiver side needs flow control or deeper buffering at ebpf2203's switch port, or several worker nodes behind the LB to spread the load. `e2sar_ft` would also need a way to set entropy, to avoid collisions.

The first sweep attempt ran while another user's 8-sender back-to-back test was running on the same nodes. Those numbers were thrown away. Always do the idle check in step 4.

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
| Sender shows `0 errors`, but the receiver gets nothing | The sender's `--ip` isn't the address the packets leave from, so the LB drops them. Check `ip route get 192.188.29.16`. Also check that inbound UDP to the receiver ports isn't firewalled |
| `e2sar_ft` or `lbadm` can't find libraries, or `EJFAT_URI` is empty | The shell didn't read `~/.bashrc`: tcsh, or a `bash -s` script. Run `bash`, or `source ~/.bashrc` |
| Large file missing, `Lost in reassembly: 1`, no drops anywhere | The event wasn't complete within `--timeout`. Use `--timeout 10000`, `--mtu 9000` and no `--smooth` |
| gRPC or certificate errors | The URI must be `ejfats://` with the hostname. Use `-v` (`--novalidate`) only if the CA isn't installed |
| Either app exits: `NUMA management not available` | This E2SAR build has no NUMA support. Drop `--numa` |
