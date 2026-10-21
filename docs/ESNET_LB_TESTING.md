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
| [1](#1-environment-bashrc-and-the-lb-instance-once) | either (shared `$HOME`) | Put the E2SAR environment and admin URI in `~/.bashrc`, reserve an LB instance and source it |
| [2](#2-build-once) | either | Build e2sar-daos |
| [3](#3-the-lb-with-lbadm-reservation-senders-workers) | either | Reservation details, inspect the LB, and add or remove senders and workers |
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
**Never copy the tokens into git.** To install E2SAR itself, see [E2SAR_INSTALL.md](E2SAR_INSTALL.md).

```bash
## E2SAR
export E2SAR_ROOT=$HOME/opt/e2sar           # E2SAR staging root
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH
export E2SAR_BIN=$E2SAR_ROOT/usr/local/bin   # not on PATH; call the tools as $E2SAR_BIN/<tool>

## ESnet EJFAT LB (secrets: keep this file private, never copy into git)
export EJFAT_ADMIN_URI='ejfats://<admin token>@ejfat-lb.es.net:18008/lb/0'   # admin token: reserve/free/overview only
```
`meson` and `ninja` must be on `PATH` too (`export PATH=$HOME/.local/bin:$PATH`).

Next, reserve an LB instance with the admin URI. `-e` prints only `export EJFAT_URI='<instance URI>'`, so the output can go straight into a file that you source:

```bash
bash                                       # from tcsh; this reads ~/.bashrc
ls -l ~/e2sar-instance.env 2>/dev/null     # must not exist: '>' would overwrite the only copy of an older instance token
(umask 077; $E2SAR_BIN/lbadm --reserve -v -u "$EJFAT_ADMIN_URI" -l <name> -a 129.57.178.85 -d 24:00:00 -e > ~/e2sar-instance.env)
cut -d@ -f2 ~/e2sar-instance.env           # expect: ejfat-lb.es.net:18008/lb/<id>?sync=...'
source ~/e2sar-instance.env
$E2SAR_BIN/lbadm --status                  # the new LB ID, with 129.57.178.85 under Registered senders
```
- `-u "$EJFAT_ADMIN_URI"`: reserving needs the admin token. The instance URI can't reserve.
- `-l <name>` is a name of your choice (it shows up in `--overview`). `-d 24:00:00` reserves it for 24 hours. `-a` puts ebpf2201 on the sender allowlist at once. The options are explained in 3.3.
- `-v` (`--novalidate`) skips the TLS certificate check, for when the CA isn't installed. If you need it here, add it to every other `lbadm` call too.
- If the file is empty or holds an error message, the reservation failed. Delete the file, fix the cause, and try again.
- `umask 077` makes the file readable only by you. It holds the instance token, which the LB never shows again: don't lose it, and never copy it into git.

Do this once per reservation, on either node, since `$HOME` is shared.

Then, in **each** terminal, source the instance and set the per-test variables. The port, container and directories differ from the node-to-node procedure in [EBPF_TESTBED_NOTES.md](EBPF_TESTBED_NOTES.md), which uses 20480 and `e2sar-test`, so the two can't collide:

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
echo "$EJFAT_URI" | cut -d@ -f2        # expect: ejfat-lb.es.net:18008/lb/<id>?sync=...
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

## 3. The LB with `lbadm`: reservation, senders, workers

An LB test goes through this life cycle:

```
reserve instance ─▶ allow senders ─▶ register worker(s) ─▶ send ─▶ deregister worker(s) ─▶ remove senders ─▶ free instance
   (3.3, once)        (3.4)              (3.5)              (7)         (3.5)                  (3.4)          (3.3, at the end)
```

With `--withcp`, the apps do the middle steps themselves. `e2sar_daos_recv` registers and deregisters its worker, and `e2sar_ft` adds and removes its sender IP. So a normal run (steps 6–8) only needs an instance URI.
The `lbadm` commands below do each step by hand: to inspect the LB, to prepare it for a sender that doesn't use `--withcp`, or to clean up after a crash.
`lbadm` reads `$EJFAT_URI` unless `-u <uri>` is given. Run it in bash after `source ~/.bashrc` and `source ~/e2sar-instance.env`.

### 3.1 Tokens and URIs

The admin URI lives in `~/.bashrc`. The instance URI lives in `~/e2sar-instance.env`, which `--reserve -e` writes (step 1).

| URI | Token | Used for |
|---|---|---|
| **Admin** `$EJFAT_ADMIN_URI`, `ejfats://<admin token>@ejfat-lb.es.net:18008/lb/0` | admin (`ALL` or `LOAD_BALANCER`) | `--reserve`, `--overview` and token management. The apps don't need it |
| **Instance** `$EJFAT_URI`, `ejfats://<instance token>@ejfat-lb.es.net:18008/lb/<id>?sync=...&data=...` | reservation, for one LB | `--status`, `--addsenders`, `--removesenders`, `--register`, `--deregister`, `--free`, and both apps |
| Session | issued to one worker by `--register` | That worker's `--state` and `--deregister`. With `-s <session id>`, the instance URI works for both |

An example instance URI, for LB 638 as used in these tests, with the token replaced by a placeholder:

```
ejfats://<instance token>@ejfat-lb.es.net:18008/lb/638?sync=192.188.29.6:19016&sync=[2001:400:a300::6]:19016&data=192.188.29.16:16384-32767&data=[2001:400:a300::16]:16384-32767
```

| Part | Example | Meaning |
|---|---|---|
| Scheme | `ejfats://` | gRPC over TLS. Use the hostname, not an IP, so the certificate validates |
| Token | `<instance token>` | Printed once by `--reserve` (3.3). It's a secret: keep it in `~/e2sar-instance.env` only |
| Control plane | `ejfat-lb.es.net:18008` | Where `lbadm` and the apps send gRPC calls |
| LB ID | `/lb/638` | The reserved instance. The admin URI uses `/lb/0` |
| `sync=` | `192.188.29.6:19016`, `[2001:400:a300::6]:19016` | Where the sender sends sync packets, IPv4 and IPv6. The port is specific to this instance |
| `data=` | `192.188.29.16:16384-32767`, `[2001:400:a300::16]:16384-32767` | The LB's data-plane address and port range, IPv4 and IPv6. The sender sends events here |

`--reserve -e` writes it wrapped in single quotes, which the shell needs because `&` and `[` mean something to it. Keep the quotes if you edit the file by hand. A different instance has a different token, LB ID and `sync=` port. Always take the URI exactly as `--reserve` printed it.

In the node-to-node case the URI needs `data=<ip>:<port>`. Here, keep the `data=` range exactly as the LB returns it. The LB picks the worker port from what the receiver registers.

### 3.2 Inspect the LB

```bash
$E2SAR_BIN/lbadm --status                            # this instance ($EJFAT_URI)
$E2SAR_BIN/lbadm -u "$EJFAT_ADMIN_URI" --overview    # every LB on ejfat-lb.es.net
```

`--status` prints:
```
   LB ID: 638
LB details: expiresat=2027-10-14T18:59:08.626Z, currentepoch=1748628, predictedeventnum=...
Registered senders: 129.57.69.58 129.57.69.28 ...
Registered workers:
[ name=ebpf2203.jlab.org, controlsignal=..., fillpercent=..., slotsassigned=512, lastupdated=..., IP Address=129.57.178.86, UDP Port=20600, ... ]
```
- `Registered senders` is the allowlist. The LB drops data from any source IP that isn't on it.
- `Registered workers` lists the receivers that get events, one line each. Nothing after the colon means no workers.
- Neither command shows a worker's session ID. Only the process that registered the worker knows it (3.5).

### 3.3 Reserve an instance (or reuse one)

**If you already have an instance** (LB 638 was reserved for the 2026-10-19 tests), put its URI in `~/e2sar-instance.env` as `export EJFAT_URI='<instance URI>'` instead of reserving, source it, check that it hasn't expired and go to 3.4:
```bash
$E2SAR_BIN/lbadm --status | grep -E 'LB ID|expiresat'
```

**To reserve a new instance** (step 1), use the admin URI and save the output:
```bash
(umask 077; $E2SAR_BIN/lbadm --reserve -v -u "$EJFAT_ADMIN_URI" -l <name> -a 129.57.178.85 -d 24:00:00 -e > ~/e2sar-instance.env)
cat ~/e2sar-instance.env
# export EJFAT_URI='ejfats://<new instance token>@ejfat-lb.es.net:18008/lb/<id>?sync=...&data=...'
```
- `-l` is a name of your choice. It shows up in `--overview`.
- `-a` puts sender IPs on the allowlist at once. Repeat it for several senders, or leave it out and use 3.4 later.
- `-d hh:mm:ss` is how long the reservation lasts: `-d 24:00:00` is 24 hours. It ends at `expiresat`, which `--status` shows. Without `-d`, `lbadm` sends `00:00:00` and the LB applies its default.
- `-e` prints only the `export EJFAT_URI=...` line. Without `-e`, the same URI is printed as `Updated URI after reserve with instance token: ...`.

Then `source ~/e2sar-instance.env` in every terminal and check with `lbadm --status`.
The instance token in that URI is the only way to manage the reservation, and the LB doesn't show it again. Don't overwrite the file while the instance is still reserved, and don't put it in git.

**When your tests are over**, free your instance. This removes every sender and worker on it. First make sure nothing is still using it (no `e2sar_daos_recv` or `e2sar_ft` running, `Registered workers:` empty), then:
```bash
source ~/e2sar-instance.env                          # EJFAT_URI: the instance to free
$E2SAR_BIN/lbadm --status | grep 'LB ID'             # check it's your instance
$E2SAR_BIN/lbadm --free                              # uses the instance token in $EJFAT_URI
#   Freeing a load balancer
#      LB ID: <id>
#   Success.
$E2SAR_BIN/lbadm -u "$EJFAT_ADMIN_URI" --overview | grep '<name>'   # expect nothing: it's gone
rm ~/e2sar-instance.env                              # only after --free printed Success.
```
If you lost the env file, free it with the admin URI and the LB ID from `--overview` (it lists each LB as `LB <name> ID: <id> ...`):
```bash
$E2SAR_BIN/lbadm -u "$EJFAT_ADMIN_URI" --overview | grep '<name>'   # find your LB ID
$E2SAR_BIN/lbadm -u "$EJFAT_ADMIN_URI" --free -i <id>
```
If you don't free it, the reservation ends by itself at `expiresat` (24 hours after `--reserve -d 24:00:00`).
Never free an instance that someone else reserved, LB 638 included, unless its owner asks you to.

### 3.4 Senders: the allowlist

**Automatic (normal case).** `e2sar_ft --withcp --ip <addr>` adds `<addr>` when it starts and removes it when it exits (step 7). Nothing else is needed.

**By hand,** for a sender that doesn't use `--withcp` (`e2sar_ft` without it, `e2sar_perf`, CODA, ...), or for one that should stay allowed between runs:
```bash
$E2SAR_BIN/lbadm --addsenders -a 129.57.178.85                     # repeat -a for more IPs
$E2SAR_BIN/lbadm --status | grep 'Registered senders'              # check
$E2SAR_BIN/lbadm --removesenders -a 129.57.178.85                  # when done
```
- Without `-a`, `lbadm` uses the address of the interface it would use to reach the LB. Pass `-a` explicitly. Use the address from `ip route get 192.188.29.16` (step 4), the one the data actually leaves from.
- Remove only the IPs you added. LB 638 also lists JLab and NERSC senders that belong to other people.
- If a `--withcp` sender was killed before it could clean up, its IP stays on the list. Remove it with `--removesenders -a <ip>`.

### 3.5 Workers: the receivers

**Automatic (normal case).** `e2sar_daos_recv --withcp` registers a worker under its hostname, with `--ip`, `--port` and the number of ports from `--threads`. It then sends a state update every few seconds and deregisters on Ctrl-C (steps 6 and 8).
The LB assigns slots once the first state update arrives. Before sending, wait until `--status` shows `slotsassigned` above 0.

**By hand,** for example to test the LB path with a plain UDP listener, no root needed:
```bash
# ebpf2203: register a probe worker on port 20650 (outside 20600-20639)
$E2SAR_BIN/lbadm --register -n probe-$USER -a 129.57.178.86 -p 20650 -w 1 -c 1
#   Success.
#   Session id is: <SID>                 <- write it down; nothing else will show it
SID=<SID>

# keep it alive: a state update at least every 10 s, or the LB drops the worker
while :; do $E2SAR_BIN/lbadm --state -s $SID -q 0 -t 0 -r 1 > /dev/null; sleep 2; done &
STATE_PID=$!

$E2SAR_BIN/lbadm --status | grep probe-$USER        # slotsassigned > 0 after a few seconds
python3 -c 'import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("129.57.178.86", 20650)); print(s.recvfrom(9000)[1])'

# remove it
kill $STATE_PID
$E2SAR_BIN/lbadm --deregister -s $SID              # the instance URI, plus -s; not a /lb/<SID> URI
```
- `--register` options: `-n` worker name, `-a` receive IP (without it, auto-detected), `-p` first UDP port, `-c` number of ports (a power of 2; the ports are `-p` to `-p`+`-c`−1), `-w` weight relative to other workers.
- `--state` options: `-q` queue fill (0–1), `-t` control signal, `-r 1` ready or `-r 0` not ready (no new events). The first update must arrive within 10 s of registering.
- While the probe is registered, it gets events from every allowlisted sender on the LB, other people's included. Keep it short.

**Removing a worker you didn't register by hand.** The LB keeps a worker only while it sends state updates:
1. If its process is still running, stop it with Ctrl-C. It deregisters itself.
2. If the process crashed or was killed (`kill -9`, lost terminal), the state updates stop and the LB drops the worker after about 10 s.
3. If you have its session ID (from `lbadm --register` output; `e2sar_daos_recv` doesn't print it), run `lbadm --deregister -s <SID>` to remove it immediately.

Check with `lbadm --status` that `Registered workers:` is empty. A worker that stays on the list without a running process is somebody else's: leave it, or ask its owner.

### 3.6 Summary: the whole flow by hand

```bash
# once, admin: reserve into the env file, then source it in every terminal
(umask 077; $E2SAR_BIN/lbadm --reserve -v -u "$EJFAT_ADMIN_URI" -l <name> -a 129.57.178.85 -d 24:00:00 -e > ~/e2sar-instance.env)
source ~/e2sar-instance.env
# per run
$E2SAR_BIN/lbadm --status                                   # 1. empty worker list (step 4)
#   ebpf2203: e2sar_daos_recv --withcp ...                  # 2. registers the worker (step 6)
$E2SAR_BIN/lbadm --status | grep slotsassigned              # 3. slots > 0
#   ebpf2201: e2sar_ft -s --withcp --ip 129.57.178.85 ...   # 4. adds sender, sends, removes sender (step 7)
#   ebpf2203: Ctrl-C                                        # 5. deregisters the worker (step 8)
$E2SAR_BIN/lbadm --status                                   # 6. no workers; same senders as in 1
# once, at the very end, your own instance only
$E2SAR_BIN/lbadm --free && rm ~/e2sar-instance.env
```

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
Through the LB this works where node-to-node didn't. E2SAR gives each event a random 16-bit entropy value, and the LB uses it to pick one of the worker's ports. With `--threads T`, the receiver opens and registers T ports, so the events land on different receive threads.
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
That's the same as node-to-node ebpf2201 → ebpf2203 (7.3–7.4 Gbps sent, 8.8–8.9 Gbps ingest), so the LB and the WAN path don't lower single-sender throughput.

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

The first sweep attempt ran while another user's 8-sender node-to-node test was running on the same nodes. Those numbers were thrown away. Always do the idle check in step 4.

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
| `lbadm --status`: `failed to connect to all addresses ... 10.10.10.10:1234` | The shell still has the node-to-node placeholder `EJFAT_URI`. Run `source ~/e2sar-instance.env` |
| `lbadm --overview`: `Error in parsing URI from command-line` | `$EJFAT_ADMIN_URI` is empty in this shell. Run `source ~/.bashrc` |
| A worker stays in `--status` after its receiver was killed | Wait about 10 s for the LB to drop it, or `lbadm --deregister -s <SID>` if you have the session ID (3.5) |
| `e2sar_ft` or `lbadm` can't find libraries, or `EJFAT_URI` is empty | The shell didn't read `~/.bashrc` or the instance file: tcsh, or a `bash -s` script. Run `bash`, then `source ~/.bashrc; source ~/e2sar-instance.env` |
| Large file missing, `Lost in reassembly: 1`, no drops anywhere | The event wasn't complete within `--timeout`. Use `--timeout 10000`, `--mtu 9000` and no `--smooth` |
| gRPC or certificate errors | The URI must be `ejfats://` with the hostname. Use `-v` (`--novalidate`) only if the CA isn't installed |
| Either app exits: `NUMA management not available` | This E2SAR build has no NUMA support. Drop `--numa` |
