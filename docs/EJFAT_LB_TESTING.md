# Testing through the ESnet EJFAT load balancer

This is the full procedure for sending files with `e2sar_ft` from ebpf2201, through the ESnet EJFAT load balancer (LB), to `e2sar_daos_recv` on ebpf2203.
The receiver writes each event into DAOS, and `e2sar_daos_read` restores the files and checks them against the source digests.
Every run uses **MTU 9000**. The sender reads from local `/scratch/<user>`, and the receiver restores into local `/scratch/<user>`.
Last run on 2026-10-19 against `feature/daos-kv-recv-apps`, using LB instance 638 on `ejfat-lb.es.net`. All three runs passed (see [Results](#results-from-2026-10-19)).

```
ebpf2201 (129.57.178.85)          ESnet LB                              ebpf2203 (129.57.178.86)
/scratch/<user>/e2sar_src_lb                                            DAOS sc26/e2sar-lb ──▶ /scratch/<user>/e2sar_out_lb
e2sar_ft -s --withcp ──UDP──▶ data 192.188.29.16:16384-32767 ──UDP──▶ e2sar_daos_recv --withcp :20600
          ├── sync UDP ─────▶ sync 192.188.29.6:19016
          └── gRPC ─────────▶ ejfat-lb.es.net:18008 ◀──────── gRPC ──── (register, state, deregister)
              (add/remove sender)
```

Workflow at a glance:

| Step | Where | What |
|---|---|---|
| [0](#0-access-and-node-facts) | laptop | Log in to both nodes |
| [1](#1-environment-each-shell-both-nodes) | both | Set the environment |
| [2](#2-build-once) | either | Build e2sar-daos |
| [3](#3-lb-reservation-and-uri) | either | Get an LB instance URI, or reserve one |
| [4](#4-pre-flight-checks-before-each-run) | both | Check the LB path, the LB state, and that nobody else is using your port |
| [5](#5-stage-source-files-ebpf2201) | ebpf2201 | Copy the source files to `/scratch` and save digests |
| [6](#6-start-the-receiver-ebpf2203) | ebpf2203 | Start the receiver; it registers with the LB |
| [7](#7-send-ebpf2201) | ebpf2201 | Send through the LB |
| [8](#8-stop-the-receiver-ebpf2203) | ebpf2203 | Stop the receiver; it deregisters |
| [9](#9-verify-and-restore-ebpf2203) | ebpf2203 | Restore from DAOS and compare digests |
| [10](#10-clean-up-required-after-every-run) | both | Delete source copies, restored files and DAOS data |

## 0. Access and node facts

The route is laptop → scilogin → tblogin → ebpf220x. scilogin asks for a password and MFA, so scripted SSH can't log in by itself.
Log in once per host and let later commands reuse the connection:

```
# ~/.ssh/config
Host scilogin tblogin ebpf* daos-adm
  ControlMaster auto
  ControlPath ~/.ssh/cm/%r@%h:%p
  ControlPersist 12h
```

```bash
mkdir -p ~/.ssh/cm && chmod 700 ~/.ssh/cm
ssh -fN ebpf2201 && ssh -fN ebpf2203          # log in once, in your own terminal
ssh -O check ebpf2201                            # "Master running"
```

| | ebpf2201 (sender) | ebpf2203 (receiver) |
|---|---|---|
| E2SAR 0.4.0rc1 (`e2sar_ft`, `lbadm`) in `~/opt/e2sar` | yes | yes |
| e2sar-daos checkout `~/daq/e2sar-daos` | shared `$HOME` | shared `$HOME` |
| DAOS 2.6.5 client, pool `sc26` | yes | yes |
| Data-plane NIC `enp1s0f1np1`, 100 Gb/s, MTU 9000 | 129.57.178.85 | 129.57.178.86 |
| `/scratch` | local XFS | local XFS |

- `$HOME` (`/testbed/home/<user>`) is NFS, shared by both nodes. `/scratch/<user>` is **local** to each node.
- The login shell is tcsh. Run every snippet below inside `bash` (or `ssh -x <host> bash -s < script.sh`).
- ebpf2203 also has an e2sar 0.3.1 RPM in `/usr/local/bin`. Always call the 0.4.0rc1 tools as `$E2SAR_BIN/...`.
- GitHub over SSH is blocked from the nodes. Use HTTPS.
- Other people run tests on these nodes. Before every run, check that your port and container are free (step 4).

## 1. Environment (each shell, both nodes)

Keep the instance URI in a file under `$HOME` that is **not** in git, for example `~/e2sar_run/lbenv.sh`, and `source` it in each shell:

```bash
export PATH=$HOME/.local/bin:$PATH
export E2SAR_ROOT=$HOME/opt/e2sar
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH
export E2SAR_BIN=$E2SAR_ROOT/usr/local/bin
export PORT=20600                      # receiver UDP port; the worker registers it with the LB
export CONT=e2sar-lb                   # DAOS container used only for LB tests
export SCR=/scratch/$USER              # node-local
export SRC=$SCR/e2sar_src_lb           # sender input (ebpf2201)
export OUT=$SCR/e2sar_out_lb           # receiver output (ebpf2203)
export RUN=$HOME/e2sar_run             # shared: env file, digests, logs
export EJFAT_URI='<instance URI from step 3>'
```

The port, container and directories differ from the back-to-back procedure in [EBPF_TESTBED_NOTES.md](EBPF_TESTBED_NOTES.md), which uses 20480 and `e2sar-test`. That way an LB run and a back-to-back run can't collide.

## 2. Build (once)

```bash
cd ~/daq/e2sar-daos
git pull https://github.com/JeffersonLab/e2sar-daos.git feature/daos-kv-recv-apps
meson setup build
meson compile -C build
meson test -C build --suite unit           # Ok: 3, Fail: 0
./build/bin/e2sar_daos_recv --help | grep withcp     # -c [ --withcp ] enable control plane interactions
```

## 3. LB reservation and URI

There are two kinds of URI. **Keep both out of git**, along with logs that echo them.

- **Admin URI**, `ejfats://<admin token>@ejfat-lb.es.net:18008/lb/0`. Only for `lbadm --reserve`, `--free`, `--overview` and token management. The apps don't need it.
- **Instance URI**, which identifies one reserved LB. It carries the instance token, the LB id, the sync addresses and the data address range:

```
ejfats://<instance token>@ejfat-lb.es.net:18008/lb/<id>?sync=192.188.29.6:19016&sync=[2001:400:a300::6]:19016&data=192.188.29.16:16384-32767&data=[2001:400:a300::16]:16384-32767
```

The apps and `lbadm --status` use the instance URI.
In the back-to-back case the URI needs `data=<ip>:<port>`, but here keep the `data=` range exactly as the LB gives it. The LB picks the worker port from what the receiver registers.

**If you already have an instance URI** (LB 638 was already reserved for these tests), put it in `EJFAT_URI` and go to step 4.

**To reserve a new instance** (not run in these tests), use the admin URI. `-a` adds sender IPs to the allowlist, `-d` sets the duration, and `-e` prints the `export EJFAT_URI=...` line for the new instance:

```bash
EJFAT_URI='<admin URI>' $E2SAR_BIN/lbadm --reserve -l <name> -a 129.57.178.85 -d 24:00:00 -e
```
When you are done with your own reservation, free it with `lbadm --free` and the instance URI.
Never free an instance that someone else reserved.

## 4. Pre-flight checks (before each run)

**LB path** (from each node, once per session):
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
Write down the sender list so you can check at the end that you left it as it was. On 2026-10-19, LB 638 already listed other JLab and NERSC senders.
The worker list should be empty. If it isn't, someone else is receiving on this LB, and events would be split between their workers and yours.

**Nobody else on your port or container** (on ebpf2203):
```bash
ss -uanp | grep ":$PORT\b"                        # expect nothing
ps -eo user,pid,etime,args | grep -E 'e2sar_(ft|daos_recv)' | grep -v grep
daos cont list sc26                               # e2sar-lb should not exist yet
```
If the port is taken, pick another port. Don't kill someone else's run.

## 5. Stage source files (ebpf2201)

Keep each file under 2 GiB so the EVIO viewer can open it. `frames_thread0_file0020.evio` (1.78 GB) qualifies.
Add one random file whose size is not a multiple of 1 MiB. Generate it once in `$RUN` and reuse it, so the digests stay the same.

```bash
mkdir -p $SRC $RUN
cp $HOME/evio-data/frames_thread0_file0020.evio $SRC/
[ -f $RUN/random_3500K.bin ] || head -c 3500K /dev/urandom > $RUN/random_3500K.bin
cp $RUN/random_3500K.bin $SRC/
(cd $SRC && sha256sum -- *) | tee $RUN/src_lb.sha256
```

## 6. Start the receiver (ebpf2203)

Use a new `PREFIX` for every send. `e2sar_ft` numbers events from 0 each time, so a second send under the same prefix overwrites the first in DAOS.
Set the same `PREFIX` value in the receiver shell and the verify shell.

```bash
export PREFIX=lb_$(date +%s)
nstat -n                                          # reset the UDP counter baseline for this run
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

With `--withcp`, the receiver registers as a worker under its hostname, with `--ip` and `--port`, then sends state updates to the LB.
Wait about 10 s, then check from another shell that the LB has given it slots:

```bash
$E2SAR_BIN/lbadm --status | grep slotsassigned
# [ name=ebpf2203.jlab.org, ..., slotsassigned=512, ..., IP Address=129.57.178.86, UDP Port=20600, ... ]
```
Don't send until `slotsassigned` is above 0.

## 7. Send (ebpf2201)

```bash
$E2SAR_BIN/e2sar_ft -s --withcp --ip 129.57.178.85 --mtu 9000 --rate 5 -p $SRC/* 2>&1 | tee $RUN/$PREFIX.send.log
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

Check that it left cleanly and that the kernel dropped nothing:
```bash
$E2SAR_BIN/lbadm --status | tail -2               # "Registered workers:" with nothing after it
nstat -z UdpRcvbufErrors                          # 0 since the nstat -n in step 6
```

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
Restored files are named `<PREFIX>_<evt>_4321.evio`. To view them, see the viewer part of [TESTING.md §4](TESTING.md#4-applications-end-to-end-on-ebpf2203).

## 10. Clean up (required, after every run)

Each run leaves the source copy on the sender, the restored files on the receiver and the event data in DAOS. Remove all three once the run is verified.
Keep `$RUN`, which holds the env file, logs, digests and `random_3500K.bin`.

```bash
# ebpf2203: restored files and the LB test container
rm -rf $OUT
daos cont destroy sc26 $CONT                      # e2sar_daos_recv recreates it on the next run
# ebpf2201: source copies
rm -rf $SRC
# check
ls /scratch/$USER                                 # on both nodes: no *_lb directories
daos cont list sc26                               # no e2sar-lb
$E2SAR_BIN/lbadm --status                         # no workers; same senders as in step 4
```

Delete only what you created. Leave other users' directories and containers alone, and other senders on the LB too.

## Scripted run from a laptop

This script drives steps 6–9 for one run over the shared SSH connections.
It assumes step 5 is done and `~/e2sar_run/lbenv.sh` (step 1) exists. Run step 10 afterwards.

```bash
#!/bin/bash
# usage: bash lbrun.sh [PREFIX]
P=${1:-lb_$(date +%s)}
ssh -x ebpf2203 bash -s <<EOF
source ~/e2sar_run/lbenv.sh; cd ~/daq/e2sar-daos
nstat -n
nohup ./build/bin/e2sar_daos_recv --withcp --ip 129.57.178.86 --port \$PORT --pool sc26 --cont \$CONT \
  --prefix $P --timeout 10000 -b 1073741824 --verbose > \$RUN/$P.recv.log 2>&1 < /dev/null &
echo \$! > \$RUN/$P.recv.pid; sleep 12
\$E2SAR_BIN/lbadm --status 2>&1 | grep -o "name=[^,]*, .*slotsassigned=[0-9]*"
EOF
ssh -x ebpf2201 bash -s <<EOF
source ~/e2sar_run/lbenv.sh
\$E2SAR_BIN/e2sar_ft -s --withcp --ip 129.57.178.85 --mtu 9000 --rate 5 -p \$SRC/* 2>&1 \
  | tee \$RUN/$P.send.log | grep -E "Queueing|goodput|frames sent"
EOF
ssh -x ebpf2203 bash -s <<EOF
source ~/e2sar_run/lbenv.sh; cd ~/daq/e2sar-daos
sleep 12; kill -INT \$(cat \$RUN/$P.recv.pid); sleep 3
grep -E "committed $P|Lost in|Ingest|DAOS commit thr" \$RUN/$P.recv.log
nstat -z UdpRcvbufErrors | tail -1
mkdir -p \$OUT/$P
./build/bin/e2sar_daos_read --cont \$CONT --prefix $P --fetch -o \$OUT/$P | tee \$OUT/$P.sha256
diff <(cut -d' ' -f1 \$RUN/src_lb.sha256 | sort) \
     <(grep -E '^[0-9a-f]{64} ' \$OUT/$P.sha256 | cut -d' ' -f1 | sort) && echo PASS || echo FAIL
EOF
```

## Results from 2026-10-19

LB 638, ebpf2201 → ebpf2203, MTU 9000, `--rate 5`, `-b 1 GiB`, `--timeout 10000`. The path to the LB had a 71 ms RTT and passed 9000-byte packets unfragmented.

| Run | Files | Sender goodput | Receive window | Ingest | DAOS commit | Result |
|---|---|---|---|---|---|---|
| 1 | 3.5 MB random only | (one small event) | | | 1 of 1 committed | PASS |
| 2 | 1.78 GB EVIO + 3.5 MB random | 7.12 Gbps | 1.53 s | 9.30 Gbps | 8.53 Gbps | PASS |
| 3 | same | 7.12 Gbps | 1.47 s | 9.70 Gbps | 9.01 Gbps | PASS |

Every run had 0 lost events, 0 `UdpRcvbufErrors` and matching digests. The receiver got all 512 slots within about 10 s and deregistered on Ctrl-C.
Throughput is the same as the back-to-back ebpf2201 → ebpf2203 runs in [EBPF_TESTBED_NOTES.md](EBPF_TESTBED_NOTES.md) (7.3–7.4 Gbps sent, 8.8–8.9 Gbps ingest).
So the LB and the WAN path don't lower throughput here. The single-threaded segmenter limit in [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md) still sets the ceiling.

## Recommended settings

| | Setting | Why |
|---|---|---|
| Both | `--withcp` | Without it, the receiver doesn't register, so the LB has nowhere to send |
| Both | Instance URI with the `data=` range as given | The LB, not the URI, chooses the worker port |
| Sender | `--ip <address the packets leave from>` | It goes on the LB sender allowlist |
| Sender | `--mtu 9000` | The LB path supports it, and at 1500 the sender is limited to ~2 Gbps |
| Sender | `--rate 5`, no `--smooth` | `--smooth` caps the sender below 1 Gbps, too slow for a 1.78 GB event in 10 s |
| Sender | `-p $SRC/*` | Required after list-valued options, which otherwise swallow the file names |
| Receiver | `-b 1073741824` | Avoids kernel socket drops (`RcvbufErrors`) |
| Receiver | `--timeout 10000` | The maximum. The default 500 ms drops multi-GB events |
| Receiver | New `--prefix` per send | Event numbers restart at 0 |

## Troubleshooting

Only the first row happened in these runs. The others are what to check first, based on how the LB and E2SAR work.

| Symptom | Cause and fix |
|---|---|
| Receiver: `Unable to open receive sockets: Address already in use` | Another receiver holds `$PORT`. Check with `ss -uanp \| grep :$PORT`, and pick another port rather than killing someone else's run |
| Receiver runs, but `lbadm --status` lists no worker | It was started without `--withcp`, or it exited. Read its output |
| Worker listed with `slotsassigned=0` | The LB hasn't recomputed the schedule yet. Wait a few seconds before sending |
| Sender shows `0 errors`, but the receiver gets nothing | The sender's `--ip` isn't the address the packets leave from, so the LB drops them as from an unknown sender. Check `ip route get 192.188.29.16`. Also check that inbound UDP to the receiver port isn't firewalled |
| Receiver gets events that you didn't send | Another allowlisted sender is sending on this LB instance. The `--prefix` keeps them apart in DAOS, but use your own reservation for clean numbers |
| Large file missing, `Lost in reassembly: 1` | The event wasn't complete within `--timeout`. Use `--timeout 10000`, `--mtu 9000` and no `--smooth` |
| Large file missing, `RcvbufErrors` > 0 | The receive socket overflowed. Use `-b 1073741824` |
| gRPC or certificate errors | The URI must be `ejfats://` with the hostname. Use `-v` (`--novalidate`) only if the CA isn't installed |
| Either app exits: `NUMA management not available` | This E2SAR build has no NUMA support. Drop `--numa` |
