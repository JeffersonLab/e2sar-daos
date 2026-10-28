# Perlmutter guide: one Perlmutter sender, two ebpf receivers, through the ESnet LB into DAOS

This is the procedure for sending synthetic events with `e2sar_perf` from **one Perlmutter compute node** at NERSC, through the ESnet EJFAT load balancer (LB), to **two receivers**, `e2sar_daos_recv` on **ebpf2201** and on **ebpf2203** at JLab.
The LB splits the events between the two receivers, and each receiver writes the events it gets into the same DAOS container.
Both receivers use the same key prefix, so the run reads back as one set of events. At the end, `e2sar_daos_read` checks that every event arrived exactly once, with the right size and content.

The commands follow the JLab procedure in [ESNET_LB_TESTING.md](ESNET_LB_TESTING.md). The Perlmutter-specific parts are marked *(check)*: confirm them when you run, and update this page with what you find.

```
Perlmutter CPU node (NERSC)                 ESnet LB (stable4)                     JLab
hsn0 128.55.x.x (not 10.x), MTU 9000                                          ebpf2201 129.57.178.85 ──▶ DAOS sc26/e2sar-pm, prefix <RUNID>
e2sar_perf -s --withcp ──UDP──▶ data 192.188.29.54:<ports> ──UDP──┬──────────────▶ e2sar_daos_recv --withcp :20600..
                                                                   └──────────────▶ e2sar_daos_recv --withcp :20600..
           └── gRPC ─────────▶ CP 192.188.29.6:18048 ◀──── gRPC ──── both workers   ebpf2203 129.57.178.86 ──▶ DAOS sc26/e2sar-pm, prefix <RUNID>
               (add/remove sender)                     (register, state, deregister)
```

| Step | Where | What |
|---|---|---|
| [1](#1-e2sar-on-perlmutter-once) | Perlmutter | E2SAR 0.4.0rc1 in `~/daq/e2sar-install`: environment in `~/.bashrc` (once) |
| [2](#2-the-lb-instance-jlab-then-perlmutter) | ebpf, then Perlmutter | The current LB instance URI (`~/e2sar_instance.env` at JLab, don't free it); copy it to Perlmutter |
| [3](#3-get-a-perlmutter-node-and-check-the-path) | Perlmutter compute node | Get a node, find the sender IP, check the path to the LB |
| [script](#the-receiver-script) | ebpf2201, ebpf2203 | `scripts/e2sar_2ebpf_recv.sh` does steps 4, 5, 7, 8 and 9 |
| [4](#4-pre-flight-checks-on-the-ebpf-nodes) | ebpf2201, ebpf2203 | LB state, nodes idle |
| [5](#5-start-the-two-receivers-ebpf2201-and-ebpf2203) | ebpf2201, ebpf2203 | Start one receiver on each; both register with the LB |
| [6](#6-send-perlmutter) | Perlmutter | Send with `e2sar_perf` |
| [7](#7-stop-the-receivers-and-check-for-drops) | ebpf2201, ebpf2203 | Stop the receivers; check for drops |
| [8](#8-verify-in-daos) | either ebpf node | Loss rate; every stored event once, right size, same digest |
| [9](#9-clean-up-required-after-every-run) | all | DAOS data, LB allowlist, the Perlmutter job |
| [10](#10-rate-sweep-optional) | all | Optional: raise the rate |

## 0. The nodes

**Perlmutter CPU node** (one example node; IPs differ per node, so look them up in step 3):

| | |
|---|---|
| OS | SUSE Linux Enterprise Server 15 SP6 (`/etc/os-release` on interactive node `nid200132`) |
| `hsn0` | Slingshot-11 (`cxi0`), 200 Gb/s, MTU 9000. Two IPv4 addresses, e.g. `10.249.44.10` (private, internal only) and `128.55.84.3` (public). **Send from the `128.x` one** |
| `nmn0` | Node management network, 1 Gb/s, MTU 1500. Not for data |
| `net.core.rmem_max`, `wmem_max` | 2,147,483,647 (~2 GiB), so a large `-b` socket buffer is allowed |
| `net.core.rmem_default`, `wmem_default` | 16 MiB |
| `net.ipv4.udp_mem` (pages) | 12098253 / 16131005 / 24196506 (~46 / 62 / 92 GiB) |
| `net.core.netdev_max_backlog` | 500,000 |
| TCP congestion control | `cubic` (only the gRPC control traffic uses TCP) |

The sender needs no DAOS: Perlmutter only runs `e2sar_perf`.

**ebpf2201 and ebpf2203** (as in [ESNET_LB_TESTING.md](ESNET_LB_TESTING.md#testing-through-the-esnet-ejfat-load-balancer)):

| | ebpf2201 (receiver A) | ebpf2203 (receiver B) |
|---|---|---|
| Data-plane NIC `enp1s0f1np1`, 100 Gb/s, MTU 9000 | 129.57.178.85 | 129.57.178.86 |
| DAOS 2.6.5 client, pool `sc26` | yes | yes |
| E2SAR 0.4.0rc1 in `$HOME/opt/e2sar`, e2sar-daos in `~/daq/e2sar-daos/build` | shared `$HOME` | shared `$HOME` |

- `$HOME` is NFS, shared by both ebpf nodes, so `~/.bashrc`, the build and `~/e2sar_run` are the same on both. Set up `~/.bashrc` and build e2sar-daos as in [ESNET_LB_TESTING.md steps 1–2](ESNET_LB_TESTING.md#1-environment-bashrc-and-the-lb-instance-once).
- The login shell is tcsh. Start `bash` in every ebpf terminal.
- Open three terminals: one on the Perlmutter compute node, one on ebpf2201 and one on ebpf2203.

## 1. E2SAR on Perlmutter (once)

**Use the same E2SAR version as the receivers, 0.4.0rc1.** The packet headers differ between E2SAR versions, and a 0.4.0rc1 receiver discards events from a sender with another header version (`Bad RE Header Discards`, see [ESNET_LB_TESTING.md step 4](ESNET_LB_TESTING.md#4-pre-flight-checks-before-each-run)).

E2SAR 0.4.0rc1 is installed without root on Perlmutter, in `$HOME/daq/e2sar-install`: the Rocky 9 `e2sar` RPM, unpacked there (as in [E2SAR_INSTALL.md section B](E2SAR_INSTALL.md)). It runs directly on the SLES 15 SP6 host, without a container *(check with the commands below)*.
`$HOME` is shared by the login and compute nodes, so install once and use it on any compute node. Perlmutter's login shell is bash, so put this in `~/.bashrc` on Perlmutter:

```bash
# e2sar installation and configuration
R=$HOME/daq/e2sar-install/usr/local
export PATH=$R/bin:$PATH
export LD_LIBRARY_PATH=$R/lib64:$R/lib:$HOME/daq/e2sar-install/usr/lib64:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$R/lib64/pkgconfig:$R/lib/pkgconfig:$PKG_CONFIG_PATH
export BOOST_ROOT=$R
# If the RPM includes Python bindings:
export PYTHONPATH=$(ls -d $R/lib*/python3*/site-packages 2>/dev/null | tr '\n' ':')$PYTHONPATH
```

Check it, on a compute node (step 3), since that's where it runs:

```bash
which e2sar_perf lbadm                            # both under ~/daq/e2sar-install/usr/local/bin
ldd $(which e2sar_perf) | grep 'not found'        # nothing: every library resolves
e2sar_perf --help | grep -E 'withcp|lbhdrversion' # the options used below
```

- `e2sar_perf` and `lbadm` are on `PATH`, so the commands below call them by name. (On the ebpf nodes they are `$E2SAR_BIN/...`.)
- Scripts started as `bash script.sh` or inside `srun` don't read `~/.bashrc`. Start them with `source ~/.bashrc`.
- If a library is missing on the SLES host, the fallback is a Rocky 9 container with the same RPM, run with `podman-hpc run --rm --net host -e EJFAT_URI <image> e2sar_perf ...` *(not tried)*.

## 2. The LB instance: JLab, then Perlmutter

**The current instance is already reserved.** Its `EJFAT_URI` is in `~/e2sar_instance.env` in the shared ebpf220x `$HOME`. It's an **instance** URI (not the admin URI): it can register workers and senders and run `--status`, but not reserve or free.

- **Don't free this instance**, and don't reserve a new one over `~/e2sar_instance.env`: it's the only copy of the instance token.
- **Add `-v` (`--novalidate`) to every command that talks to the LB**: `lbadm`, `e2sar_perf` and `e2sar_daos_recv`, on both the ebpf nodes and Perlmutter. The commands below already include it.

**ebpf2201 or ebpf2203:**

```bash
bash
source ~/e2sar_instance.env
$E2SAR_BIN/lbadm -v --status               # the LB ID; no workers between runs
cut -d@ -f2 ~/e2sar_instance.env           # must be 192.188.29.6:18048/... with data=192.188.29.54: stable4
```

Don't use the instance if someone else's workers are registered on it: the LB would give part of your events to them. `e2sar_perf --withcp` adds Perlmutter to the sender allowlist when it starts (step 6).

Only if you ever need a **new** instance (this needs the admin URI and stays at JLab), write it to another file so the current one is kept ([ESNET_LB_TESTING.md 3.3](ESNET_LB_TESTING.md#33-reserve-an-instance-or-reuse-one)):

```bash
ls -l ~/e2sar_instance_new.env 2>/dev/null # must not exist: '>' would overwrite it
(umask 077; $E2SAR_BIN/lbadm --reserve -v -u "$EJFAT_ADMIN_URI" -l pm-$USER -d 24:00:00 -e > ~/e2sar_instance_new.env)
```

**Copy the instance file to Perlmutter.** The ebpf nodes can't reach NERSC by SSH, so copy it through your own machine and delete the local copy afterwards:

```bash
# on your laptop (NERSC needs sshproxy / MFA)
scp <jlab login>:e2sar_instance.env /tmp/e2sar_instance.env
scp /tmp/e2sar_instance.env perlmutter.nersc.gov:e2sar_instance.env && rm /tmp/e2sar_instance.env
ssh perlmutter.nersc.gov chmod 600 e2sar_instance.env
```

The file holds the instance token. Keep it mode 600 and never put it in git.

## 3. Get a Perlmutter node and check the path

```bash
# Perlmutter login node
salloc -N 1 -C cpu -q interactive -t 01:00:00 -A <account>
# now on the compute node
source ~/e2sar_instance.env
echo "$EJFAT_URI" | cut -d@ -f2          # expect: 192.188.29.6:18048/lb/<id>?sync=...&data=192.188.29.54:...
```

**The sender IP: always the public `128.x` IPv4 address of `hsn0`, never the `10.x` one.** `hsn0` has two IPv4 addresses. The `10.249.x.x` one is private to NERSC and can't be reached from outside. The `128.55.x.x` one is public, and it's the one the LB has to see on its allowlist.

```bash
ip -4 -br addr show hsn0                  # both addresses, e.g. 10.249.44.10/.. 128.55.84.3/..
export SEND_IP=$(ip -4 -o addr show hsn0 | awk '{print $4}' | cut -d/ -f1 | grep '^128\.')
echo $SEND_IP                             # one 128.55.x.x address; empty means this node has none: stop
```

- Pass `--ip $SEND_IP` to `e2sar_perf` (step 6) and, if you add the sender by hand, `-v -a $SEND_IP` to `lbadm`.
- If `ip route get` shows `src 10.249.x.x`, the kernel would send from the private address, and the LB would drop the packets as coming from an unknown sender. Check that `e2sar_perf --ip $SEND_IP` makes them leave from the `128.x` address *(check)*: the receivers getting events in step 6 shows it does. If they get nothing, ask NERSC how a job sends external UDP from the public address.

**The path to the LB:**

```bash
timeout 5 bash -c '</dev/tcp/192.188.29.6/18048' && echo CP-OK        # control plane (gRPC)
ping -c3 -M do -s 8972 192.188.29.xx                                  # does MTU 9000 pass unfragmented? note the RTT
ping -c3 -M do -s 1472 192.188.29.xx                                  # MTU 1500, if 9000 fails
lbadm -v --status                                                     # the instance, as seen from Perlmutter
```

Ping the data plane in **your instance's URI** (`data=` in `cut -d@ -f2 ~/e2sar_instance.env`), not just the address above. The control plane and the data plane are separate: `lbadm` can work while the data plane is unreachable. Then the ping gets `Destination Host Unreachable`, every data packet is dropped, and the receivers get nothing.

- If the 8972-byte ping fails but the 1472-byte one works, use `--mtu 1500` in step 6 *(check)*. Throughput per send thread is then much lower.
- If the pings get no answer at all, ICMP may be filtered. The first real test is step 6 with a small number of events.
- Write down the RTT. Through the LB, an event's latency is mostly this round trip.

## The receiver script

[`scripts/e2sar_2ebpf_recv.sh`](../scripts/e2sar_2ebpf_recv.sh) does steps 4, 5, 7, 8 and 9 on the ebpf nodes. The manual commands follow below, for reference and for when something needs a closer look.

```bash
# ebpf2201 first, then ebpf2203 (each in bash, in its own terminal)
~/daq/e2sar-daos/scripts/e2sar_2ebpf_recv.sh start  # checks, run directory, receiver; Ctrl-C after the send
# a second ebpf terminal
~/daq/e2sar-daos/scripts/e2sar_2ebpf_recv.sh status # both workers with slotsassigned > 0: now send (step 6)
# after Ctrl-C on both receivers, once, on either node
~/daq/e2sar-daos/scripts/e2sar_2ebpf_recv.sh verify 10000   # N = the sender's -n; loss rate, then PASS or FAIL
~/daq/e2sar-daos/scripts/e2sar_2ebpf_recv.sh runs        # the runs in DAOS: committed, failed and lost events per run
~/daq/e2sar-daos/scripts/e2sar_2ebpf_recv.sh clean <RUNID>  # delete that run, show the LB state ('clean' alone: the whole container)
```

- `start` reads the instance URI from `~/e2sar_instance.env` and listens on `$MY_IP` from `~/.bashrc` (else the address of `enp1s0f1np1`). It stops if an e2sar process is already running, a receive port is in use, or someone else's worker is on the LB instance.
- On ebpf2201, `start` makes the new run directory `$RUN/pm_<date>_<time>`. On ebpf2203 it waits up to 5 minutes for that directory and uses the same `RUNID` as the DAOS prefix.
- Ctrl-C stops only the receiver. Its final statistics stay in `<node>.recv.log`, and the script then prints step 7's drop checks for that node.
- Each run is its own DAOS object, which both receivers share, and is listed in the container's run catalog with each receiver's final statistics. `verify` takes the receivers' commit counts from there.
- Every LB command uses `-v`. The script never reserves or frees the instance.
- Settings such as `CONT`, `PORT`, `THREADS`, `DEQ` and `RECV_ARGS` can be overridden with environment variables, e.g. `DEQ=16 .../e2sar_2ebpf_recv.sh start`. See the top of the script.

## 4. Pre-flight checks on the ebpf nodes

On **both** ebpf2201 and ebpf2203, in bash:

```bash
source ~/e2sar_instance.env
export PORT=20600                          # first receive port on each node (the LB uses each worker's own IP:port)
export CONT=e2sar-pm                       # DAOS container for these tests only, shared by both receivers
export RUN=$HOME/e2sar_run/pm; mkdir -p $RUN
export ME=$(hostname -s)                   # node name, for the log file names
echo $NIC $MY_IP                           # confirm NIC and IP addresses (both from ~/.bashrc)

ps -eo user,pid,etime,args | grep -E 'e2sar_(ft|perf|daos_recv)' | grep -v grep   # expect nothing
ss -uanp | grep -E ':20(4[89]|6[0-3])[0-9]\b'                                    # nothing on 20480-20499 or 20600-20639
daos cont list sc26 | grep $CONT                                                  # nothing: the container doesn't exist yet
```

And once, on either node:

```bash
$E2SAR_BIN/lbadm -v --status
```

`Registered workers:` must be empty. Write down `Registered senders:`, to check at the end that you left it as it was.
If either node is busy with someone else's test, wait. Concurrent tests share the NICs, the CPUs and DAOS, and spoil each other's numbers.

## 5. Start the two receivers (ebpf2201 and ebpf2203)

Every run gets its own directory, `$RUN/<RUNID>`, named after the date and time it started, such as `$RUN/pm_20261027_143005/`. Both nodes write their logs and NIC counters into it, named after the node (`ebpf2201.recv.log`, `ebpf2203.recv.log`, ...).
Both nodes need the **same** `RUNID`. ebpf2201 makes it and points `$RUN/latest` at the new directory; ebpf2203 reads it from there (`$RUN` is in the shared `$HOME`). So run the block on ebpf2201 first.
In DAOS, **both receivers use one prefix, `$RUNID`** (no `/` allowed). That's safe: the LB sends each event to only one worker, and the keys are `<prefix>/<evt>/<dataId>/<chunk>`, so the two receivers never write the same key. One `e2sar_daos_read --prefix $RUNID` then sees the whole run.
Which node got which event isn't recorded in DAOS. Each receiver's `Events committed` (step 7) gives the split.
`e2sar_perf` numbers events from 0 on every send, so use a new `RUNID` for every send.

```bash
# ebpf2201 only, first: a new run directory named by date and time
[ $ME = ebpf2201 ] && D=$RUN/pm_$(date +%Y%m%d_%H%M%S) && mkdir $D && ln -sfn $(basename $D) $RUN/latest
# both nodes
export RUNID=$(readlink $RUN/latest)       # e.g. pm_20261027_143005: the same on both nodes
export D=$RUN/$RUNID                       # this run's directory
export PREFIX=$RUNID                      # DAOS key prefix: the same for all receivers, no '/'
ls -ld $D                                  # exists, and is from just now
nstat -n
ethtool -S $NIC | grep -E ' (rx_packets_phy|rx_out_of_buffer|rx_discards_phy):' > $D/$ME.nic0
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_recv -v --withcp --ip $MY_IP \
    --port $PORT --threads 16 --pool sc26 --cont $CONT --prefix $PREFIX \
    --timeout 10000 -b 1073741824 --deq 32 --inflight 256 \
    --timing-csv $D/$ME.csv 2>&1 | tee $D/$ME.recv.log
```

Expected startup output on each node:
```
DAOS pool/container:  sc26/e2sar-pm (created)      # or (existing) on the node that came second
Run catalog:          new run                      # or "joined run created by ebpf2201..." on the second node
KV object id:         0x...e25a0001.0x...          # the same on both nodes: one object for the run
Control plane:        ON
Listening on:         129.57.178.8x ports 20600:20615
Receiving; Ctrl-C to stop
```

- Starting both at the same time is fine: if both try to create the container, the second one opens it.
- Each receiver registers as its own worker, named after its hostname, with 16 ports (`--threads 16`, 20600–20615). `-v` skips the certificate check, as for every LB command (step 2).

After about 10 s, check from a second ebpf terminal that **both** workers have slots:

```bash
$E2SAR_BIN/lbadm -v --status | grep -E 'name=ebpf220[13]' | grep -o 'name=[^,]*\|slotsassigned=[0-9]*'
# name=ebpf2201.jlab.org  slotsassigned=<n>
# name=ebpf2203.jlab.org  slotsassigned=<n>
```

Don't send until both show `slotsassigned` above 0. The two workers have the same weight, so each should get about half of the slots, and half of the events.

## 6. Send (Perlmutter)

Start with a short, slow run to prove the path, then a longer one:

```bash
cd $SCRATCH
# 1. smoke test: 100 events of 1 MiB at 1 Gbps
e2sar_perf -v -s --withcp --ip $SEND_IP --mtu 9000 -l 1048576 -n 100 --rate 1 \
    --sockets 4 -b 67108864

# 2. the run: 10,000 events of 1 MiB (10.5 GB) at 5 Gbps
e2sar_perf -v -s --withcp --ip $SEND_IP --mtu 9000 -l 1048576 -n 10000 --rate 5 \
    --sockets 4 -b 67108864

# 3. the run: 10,000 events of 1 MiB (10.5 GB) at 20 Gbps
e2sar_perf -v -s --withcp --ip $SEND_IP --mtu 9000 -l 1048576 -n 10000 --rate 20 \
    --sockets 8 -b 67108864
```

Each run needs its own `RUNID` on the receivers (step 5), so restart the receivers between the two sends, or note that the smoke test's events 0–99 get overwritten by the second run's events 0–99 under the same prefix.

Expected output:
```
Adding senders to LB: 128.55.x.x ... done
Control plane:                 ON
...
Sending sockets/threads:       4
Using LB Header Version:       2
Sending average bit rate is:   5 Gbps (with 1048576 B line-rate bursts)
Event size is 1048576 bytes or 8388608 bits
Sending 10000 event buffers
...
Completed, <packets> packets sent, 0 errors
Estimated goodput (Gbps): <about 5>
Removing senders: 128.55.x.x
```

- With `--withcp`, `e2sar_perf` adds `--ip` to the LB allowlist when it starts and removes it when it exits. If it's killed before that, remove it by hand (step 9).
- Run only one `--withcp` sender per IP at a time: the first one to exit removes the shared IP while the other is still sending.
- `-l` is the event size in bytes, `-n` the number of events, `--rate` the total send rate in Gbps (`-1`: no limit). `--sockets` is the number of send threads: 4 for up to 10 Gbps (`e2sar_perf`'s default) and 8 above that. `-b` is the send socket buffer; the node allows up to 2 GiB.
- Every event has the same content: one buffer, with `This is a start of event payload` at its start and `...the end` at its end, reused for all events. Step 8 uses that. Don't pass `--realmalloc`, which gives each event a new buffer with undefined content in the middle.
- Keep `-v` (`--novalidate`), as for every LB command (step 2). If the MTU check in step 3 failed, use `--mtu 1500`.

On the ebpf nodes, each receiver prints its progress every second. Between them, they should account for all events.

## 7. Stop the receivers and check for drops

Wait until neither receiver has committed anything new for a few seconds, then press **Ctrl-C on both**. Each one deregisters and prints its statistics, which `tee` also saves in `$D/<node>.recv.log`:

```
	Lost in reassembly:        0
	Lost in enqueue:           0
	Events committed:          <about half of -n>
	Events failed:             0
```

On each node:

```bash
nstat -z UdpRcvbufErrors                          # kernel socket drops since step 5: 0
ethtool -S $NIC | grep -E ' (rx_packets_phy|rx_out_of_buffer|rx_discards_phy):' > $D/$ME.nic1
paste $D/$ME.nic0 $D/$ME.nic1 | awk '{print $1, "+" $4-$2}'   # rx_out_of_buffer, rx_discards_phy: +0
```

And once:
```bash
$E2SAR_BIN/lbadm -v --status | tail -3               # "Registered workers:" with nothing after it; Perlmutter's IP gone from the senders
```

## 8. Verify in DAOS

**Once, on either node**, after both receivers have stopped. The whole run is under one prefix, so one read covers both receivers. `--fetch` reads every event and prints its SHA-256 without writing files. Set `N` to the `-n` of the send:

```bash
cd ~/daq/e2sar-daos
N=10000; L=1048576
./build/bin/e2sar_daos_read --cont $CONT --prefix $PREFIX > $D/events.list
tail -1 $D/events.list                     # committed events and bytes under '<RUNID>/'
awk -F'\t' 'NF==5 && $1 ~ /^[0-9]+$/ {g++} END {printf "%d of %d events (%d lost, %.2f%%)\n", g, '$N', '$N'-g, 100*('$N'-g)/'$N'}' $D/events.list
comm -23 <(seq 0 $((N-1)) | sort) <(awk -F'\t' 'NF==5 && $1 ~ /^[0-9]+$/ {print $1}' $D/events.list | sort -u) | sort -n > $D/missing.txt   # the lost event numbers
awk -F'\t' 'NF==5 && $1 ~ /^[0-9]+$/ && $3!='$L $D/events.list | wc -l                  # 0: every event is L bytes
./build/bin/e2sar_daos_read --cont $CONT --prefix $PREFIX --fetch > $D/events.sha
tail -1 $D/events.sha                      # all stored events verified, 0 failed
grep -E '^[0-9a-f]{64} ' $D/events.sha | cut -d' ' -f1 | sort | uniq -c  # ONE line: "  <stored> <digest>"
grep -H 'Events committed' $D/ebpf220[13].recv.log                     # the split; adds up to the events in DAOS
```

**The loss rate is reported, not a failure.** Events lost on the way (reassembly loss, see [Troubleshooting](#troubleshooting)) never reach DAOS, so they show up only as missing numbers in `$D/missing.txt`. Record the rate with the run.
**PASS** when every stored event is correct: numbered below N, `L` bytes, a single digest over all of them (every event was the same buffer), and the two receivers' `Events committed` add up to exactly the number of events in DAOS.
That last check catches an event delivered to both receivers: with one prefix the second write overwrites the first, so DAOS counts it once but the two receivers count it twice.
To look inside one event: `./build/bin/e2sar_daos_read --cont $CONT --prefix $PREFIX --fetch --event 0 -o /tmp/$USER-ev -e bin`, then `head -c 32` and `tail -c 10` on the file in `/tmp/$USER-ev`, which should show the start and end markers from step 6.

## 9. Clean up (required, after every run)

```bash
# either ebpf node: this run's data (both receivers wrote into the run's one object)
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_read --cont $CONT --prefixes               # the runs in the container
./build/bin/e2sar_daos_read --cont $CONT --prefix $PREFIX --delete
# or all runs at once: the whole test container
daos cont destroy sc26 $CONT
$E2SAR_BIN/lbadm -v --status                      # no workers; the senders as in step 4
# only if Perlmutter's IP is still listed (sender was killed):
$E2SAR_BIN/lbadm -v --removesenders -a <SEND_IP>
```

- Keep the run directories in `$RUN` (`$RUN/pm_<date>_<time>/`: logs, NIC counters, lists, digests). They are small. `ls $RUN` lists the runs in time order, and `$RUN/latest` points at the newest.
- On Perlmutter, `exit` the `salloc` shell to give the node back.
- **Don't free the LB instance** and don't delete `~/e2sar_instance.env` at JLab: the current instance stays reserved for later runs (step 2).

## 10. Rate sweep (optional)

Repeat steps 5–9 with a new `RUNID` each time, raising `--rate`. Keep `-l 1048576` and `-n` large enough for a run of at least 10 s (`-n` = rate in Gbps × 10 s × 10⁹ / 8 / 1048576, e.g. 12,000 at 10 Gbps).

| Rate (Gbps) | `--sockets` | Runs | Sender goodput | Committed (ebpf2201 / ebpf2203) | Lost | `rx_discards_phy` (2201 / 2203) | Result |
|---|---|---|---|---|---|---|---|
| 1 | 4 | | | | | | |
| 5 | 4 | | | | | | |
| 10 | 4 | | | | | | |
| 20 | 8 | | | | | | |
| 30 | 8 | | | | | | |
| 40 | 8 | | | | | | |

What to look at:
- 1 MiB events leave each send thread as ~10 Gbps line-rate bursts. More `--sockets` spread the rate over more threads.
- If the sender reports its rate with 0 errors but both receivers see loss with 0 NIC discards, the loss is on the way: NERSC's egress, the WAN or the LB.
- Run each rate at least twice, and record the time and the Slurm job ID with each result.

## Recommended settings

| | Setting | Why |
|---|---|---|
| Sender | E2SAR 0.4.0rc1, the receivers' version | Other versions' headers are discarded by the receivers |
| Sender | `--withcp --ip <hsn0 128.x address>`, never the `10.x` one | The `10.x` address is private; the LB allowlists and sees the public `128.x` one |
| Sender | `--mtu 9000`, if the 8972-byte ping passes | Far fewer packets per event |
| Sender | `-b 67108864` or larger | The node allows up to 2 GiB |
| Sender | One `--withcp` process per sender IP | Each one removes the shared IP when it exits |
| Sender | No `--realmalloc` | Step 8's single-digest check needs every event to be the same buffer |
| Receivers | `--withcp`, the same `--port`, `--cont` and `--prefix $RUNID` on all of them | Each node registers its own worker. The LB gives each event to one worker, so one prefix holds the whole run without collisions |
| Receivers | `--threads 16`, `--deq 32 --inflight 256`, `-b 1073741824` | 16 ports spread the events over more receive threads. 32 dequeue threads, with 256 DAOS puts in flight each, keep DAOS ahead of the network. A 1 GiB socket buffer avoids socket drops |
| Receivers | `--timeout 10000` | The maximum. Covers WAN jitter and slow starts; the default 500 ms is tight over the WAN |
| Both | New `RUNID` per send | `e2sar_perf` restarts event numbers at 0 |

## Troubleshooting

What to check first, based on how the LB and E2SAR work.

| Symptom | Cause and fix |
|---|---|
| `e2sar_perf: command not found`, or `error while loading shared libraries` | The shell didn't read `~/.bashrc` (step 1): run `source ~/.bashrc`. If a library is still missing, `ldd $(which e2sar_perf) \| grep 'not found'` names it |
| `e2sar_perf` hangs or fails at `Adding senders to LB` | The control plane isn't reachable from the node (check `CP-OK` in step 3), or the URI in this shell is empty: `source ~/e2sar_instance.env` |
| gRPC or certificate errors | A command is missing `-v` (`--novalidate`). Every `e2sar_perf`, `lbadm` and receiver command needs it (step 2) |
| Sender says `0 errors`, both receivers get nothing | `--ip` is the `10.x` address, or the packets leave from it. Use the `hsn0` `128.x` address (step 3), and check `lbadm -v --status` lists it under `Registered senders` while sending. Also check that the receivers had `slotsassigned` > 0 |
| Only one receiver gets events | The other had no slots yet, or wasn't registered (`lbadm -v --status`). Wait for both before sending |
| Every event lands on one port of a receiver | The events carry the same entropy. Check with `ss -uanm \| grep -A1 ':206'` while receiving. Still correct, but one receive thread does all the work |
| Events lost, 0 socket drops and 0 NIC discards on both nodes | The packets were lost before JLab: on Perlmutter, NERSC egress, the WAN or the LB. Lower `--rate`, check the MTU (step 3) |
| Events lost with `rx_discards_phy` > 0 | The receiving NIC port overflowed. Lower `--rate` or use more `--sockets` |
| Digest check shows more than one line | An event was corrupted, or the sender used `--realmalloc`. Compare the odd digests' event numbers with the receiver logs |
| The receivers' `Events committed` add up to more than N | Either two sends ran under the same `RUNID` (use a new one per send, step 5), or the LB delivered some events to both workers. With one prefix, the later write overwrote the earlier one |
| Receiver aborts right after `Dequeue threads:` with `boost::wrapexcept<boost::system::system_error>` / `Invalid argument [system:22]` | `--ip` isn't an IP address: `$MY_IP` is empty, holds an interface name, or is stale. Check `echo $MY_IP` (step 4) and its line in `~/.bashrc` |
| `Key prefix:` at startup shows another node's name or an old run | `PREFIX` is left over in this shell. Set `RUNID` and `PREFIX` again as in step 5; `PREFIX` must equal `$RUNID` |
| Receivers get 0 events, LB shows `totalPacketsRecv=0`, but `lbadm` works | The instance's data plane is unreachable. Ping its `data=` address from Perlmutter: `Destination Host Unreachable` means the packets are dropped on the way. Check the URI in `~/e2sar_instance.env` is on stable4 (step 2); if not, reserve one on stable4 (data plane `192.188.29.54`) with an admin URI on `192.188.29.6:18048` |
| A worker stays registered after a receiver was killed | Wait about 10 s for the LB to drop it ([ESNET_LB_TESTING.md 3.5](ESNET_LB_TESTING.md#35-workers-the-receivers)) |
