---
type: Reference
title: "The LB with lbadm: reservation, senders, workers"
description: "EJFAT LB tokens and URIs, inspecting the LB, reserving and freeing an instance, the sender allowlist and workers, with lbadm."
tags: [lbadm, lb, ejfat]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/ESNET_LB_TESTING.md", title: "ESNET_LB_TESTING.md at 0d85d00" }
---

# 3. The LB with `lbadm`: reservation, senders, workers

An LB test goes through this life cycle:

```
reserve instance ─▶ allow senders ─▶ register worker(s) ─▶ send ─▶ deregister worker(s) ─▶ remove senders ─▶ free instance
   (3.3, once)        (3.4)              (3.5)              (7)         (3.5)                  (3.4)          (3.3, at the end)
```

With `--withcp`, the apps do the middle steps themselves. `e2sar_daos_recv` registers and deregisters its worker, and `e2sar_ft` adds and removes its sender IP. So a normal run (steps 6–8) only needs an instance URI.
The `lbadm` commands below do each step by hand: to inspect the LB, to prepare it for a sender that doesn't use `--withcp`, or to clean up after a crash.
`lbadm` reads `$EJFAT_URI` unless `-u <uri>` is given. Run it in bash after `source ~/.bashrc` and `source ~/e2sar-instance.env`.

## 3.1 Tokens and URIs

The admin URI lives in `~/.bashrc`. The instance URI lives in `~/e2sar-instance.env`, which `--reserve -e` writes (step 1).

| URI | Token | Used for |
|---|---|---|
| **Admin** `$EJFAT_ADMIN_URI`, `ejfats://<admin token>@192.188.29.6:18048/lb/0` | admin (`ALL` or `LOAD_BALANCER`) | `--reserve`, `--overview` and token management. The apps don't need it |
| **Instance** `$EJFAT_URI`, `ejfats://<instance token>@192.188.29.6:18048/lb/<id>?sync=...&data=...` | reservation, for one LB | `--status`, `--addsenders`, `--removesenders`, `--register`, `--deregister`, `--free`, and both apps |
| Session | issued to one worker by `--register` | That worker's `--state` and `--deregister`. With `-s <session id>`, the instance URI works for both |

An example instance URI, for LB 638 on the older control plane as used in these tests, with the token replaced by a placeholder. On stable4 the host is `192.188.29.6:18048` and `data=` is `192.188.29.54`; take the rest exactly as `--reserve` prints it:

```
ejfats://<instance token>@ejfat-lb.es.net:18008/lb/638?sync=192.188.29.6:19016&sync=[2001:400:a300::6]:19016&data=192.188.29.16:16384-32767&data=[2001:400:a300::16]:16384-32767
```

| Part | Example | Meaning |
|---|---|---|
| Scheme | `ejfats://` | gRPC over TLS. stable4 is addressed by IP; if its certificate doesn't validate for that IP, add `-v` (`--novalidate`) to `lbadm`, `e2sar_ft` and `e2sar_daos_recv` |
| Token | `<instance token>` | Printed once by `--reserve` (3.3). It's a secret: keep it in `~/e2sar-instance.env` only |
| Control plane | `ejfat-lb.es.net:18008` (stable4: `192.188.29.6:18048`) | Where `lbadm` and the apps send gRPC calls |
| LB ID | `/lb/638` | The reserved instance. The admin URI uses `/lb/0` |
| `sync=` | `192.188.29.6:19016`, `[2001:400:a300::6]:19016` | Where the sender sends sync packets, IPv4 and IPv6. The port is specific to this instance |
| `data=` | `192.188.29.16:16384-32767`, `[2001:400:a300::16]:16384-32767` (stable4: `192.188.29.54`) | The LB's data-plane address and port range, IPv4 and IPv6. The sender sends events here |

`--reserve -e` writes it wrapped in single quotes, which the shell needs because `&` and `[` mean something to it. Keep the quotes if you edit the file by hand. A different instance has a different token, LB ID and `sync=` port. Always take the URI exactly as `--reserve` printed it.

In the node-to-node case the URI needs `data=<ip>:<port>`. Here, keep the `data=` range exactly as the LB returns it. The LB picks the worker port from what the receiver registers.

## 3.2 Inspect the LB

```bash
$E2SAR_BIN/lbadm --status                            # this instance ($EJFAT_URI)
$E2SAR_BIN/lbadm -u "$EJFAT_ADMIN_URI" --overview    # every LB on this control plane (stable4)
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

## 3.3 Reserve an instance (or reuse one)

**If you already have an instance** on stable4, put its URI in `~/e2sar-instance.env` as `export EJFAT_URI='<instance URI>'` instead of reserving, source it, check that it hasn't expired and go to 3.4:
```bash
$E2SAR_BIN/lbadm --status | grep -E 'LB ID|expiresat'
```

**To reserve a new instance** (step 1), use the admin URI and save the output:
```bash
(umask 077; $E2SAR_BIN/lbadm --reserve -v -u "$EJFAT_ADMIN_URI" -l <name> -a 129.57.178.85 -d 24:00:00 -e > ~/e2sar-instance.env)
cat ~/e2sar-instance.env
# export EJFAT_URI='ejfats://<new instance token>@192.188.29.6:18048/lb/<id>?sync=...&data=192.188.29.54:...'
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

## 3.4 Senders: the allowlist

**Automatic (normal case).** `e2sar_ft --withcp --ip <addr>` adds `<addr>` when it starts and removes it when it exits (step 7). Nothing else is needed.

**By hand,** for a sender that doesn't use `--withcp` (`e2sar_ft` without it, `e2sar_perf`, CODA, ...), or for one that should stay allowed between runs:
```bash
$E2SAR_BIN/lbadm --addsenders -a 129.57.178.85                     # repeat -a for more IPs
$E2SAR_BIN/lbadm --status | grep 'Registered senders'              # check
$E2SAR_BIN/lbadm --removesenders -a 129.57.178.85                  # when done
```
- Without `-a`, `lbadm` uses the address of the interface it would use to reach the LB. Pass `-a` explicitly. Use the address from `ip route get 192.188.29.54` (step 4), the one the data actually leaves from.
- Remove only the IPs you added. LB 638 also lists JLab and NERSC senders that belong to other people.
- If a `--withcp` sender was killed before it could clean up, its IP stays on the list. Remove it with `--removesenders -a <ip>`.

## 3.5 Workers: the receivers

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

## 3.6 Summary: the whole flow by hand

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
