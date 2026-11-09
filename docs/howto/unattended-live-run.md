---
type: How-To
title: "Keep a live run up unattended: receiver watchdog and hourly rotation"
description: "Run the live pipeline on one ebpf node with nobody watching: a watchdog restarts a crashed receiver (and the pages), and an hourly job starts a fresh run and deletes runs that got no events. Why it runs on the node, the guards that keep it from fighting the pipeline, the tests, and the two scripts."
tags: [operations, watchdog, e2sar_live_pipeline, e2sar_daos_recv, monitoring, ebpf-testbed, lb]
sources:
  - { id: pipeline, resource: "../../scripts/e2sar_live_pipeline.sh", title: "scripts/e2sar_live_pipeline.sh" }
  - { id: live, resource: "live-monitoring.md", title: "Live monitoring of a run from a laptop" }
  - { id: lb682, resource: "../results/2026-11-09-lb682-perlmutter-losses.md", title: "LB 682: receiver crashes and losses (2026-11-09)" }
---

# Keep a live run up unattended: receiver watchdog and hourly rotation

Set up on 2026-11-09 for the Perlmutter run on LB 682, when the receivers kept crashing ([LB 682 result](../results/2026-11-09-lb682-perlmutter-losses.md)) and nobody would be at the laptop. Everything runs on **ebpf2201**: receiver, reader, DAOS monitor, both pages, and the two helpers below. They live in `~/e2sar_run/perlmutter/` on the nodes (shared `$HOME`), not in the repo; their text is at the end of this page.

| Piece | What it does | Log |
|---|---|---|
| `keepalive.sh recv 129.57.178.85` | Restarts the receiver when it dies without a clean stop | `keepalive_ebpf2201.log` |
| (same, receiver mode) | Restarts the NIC sampler (`e2sar_nicmon.sh`) whenever the run's receiver is up and the sampler isn't | same |
| `keepalive.sh pages` | Restarts the monitor page (:8766) or the EVIO viewer (:8765) when one is down | same |
| `hourly.sh` | Every hour: stops the run, deletes it if it got no events (container, reader folder, monitor file, logs), starts a fresh one | `hourly.log` |
| `bin/ssh` | Stand-in for `ssh` so `e2sar_live_pipeline.sh` can drive ebpf2201 from ebpf2201 | — |

Runs are named `e2sar_perlmutter_lb682_<UTC epoch s>`: the source and the LB are in the name.

## Why on the node, and why one node

- The laptop sleeps (lid closed), loses its connection, or is off. `caffeinate -ims` only keeps a Mac awake on AC power with the lid open. A watchdog on the laptop stops with it.
- `e2sar_live_pipeline.sh` reaches every node over SSH, and the ebpf nodes can't SSH to each other or to themselves (password prompt). So a node can only drive the pipeline if receiver and reader are on that same node, with `bin/ssh` turning `ssh -x ebpf2201 bash -s -- ARGS` into `bash -s -- ARGS`.
- So the receiver, reader, monitor and pages all go on ebpf2201 (`RECV=ebpf2201:… READER=ebpf2201`, its own `/scratch/xmei`). The pages move with it: forward from the laptop with `ssh -fN -L 8766:localhost:8766 -L 8765:localhost:8765 ebpf2201`. If an old connection to ebpf2203 still holds those ports, the forward fails with `Port forwarding failed`: cancel it first (`ssh -O cancel -L 8766:localhost:8766 -L 8765:localhost:8765 ebpf2203`).

## The receiver watchdog

Every 5 s it reads the current run from `current` and looks for that run's `e2sar_daos_recv`. It restarts the receiver, joined to the same run (it claims the next writer slot; slots can't be reused), only when all of these hold:

1. **No receiver process for the run and its UDP port (20700) is free, on two checks 5 s apart.** A receiver that is still starting up isn't running yet but already binds its port. Without this guard the watchdog once restarted a receiver that `start` had just launched: the second one got `Unable to open receive sockets: Address already in use` and exited, but had already claimed writer slot 1, which then showed as a dead writer.
2. **The run is still the current one.**
3. **The receiver log is still there.** `e2sar_live_pipeline.sh stop` moves the logs to `<run>/`.
4. **The log doesn't end with `Final watermark and statistics`.** That line is what a clean stop (Ctrl-C) writes; a crash writes nothing, or a glibc abort such as `corrupted size vs. prev_size`. So `stop` and the hourly rotation are never undone.

**Crash loop.** After 3 restarts within 5 minutes it waits 5 minutes before the next. On LB 682 the receivers crashed ~20 s after each start; without the pause each restart added a writer slot (~180 an hour) and the monitor page filled with dead writers.

Each restart is logged in `keepalive_<host>.log` (with the receiver's last line) and marked in the receiver log as `[keepalive] receiver died; restart N`.

## NIC monitoring

`e2sar_live_pipeline.sh start` starts `e2sar_nicmon.sh` on every receiver node and `stop` stops it, so every run gets its 1 s NIC, IP and UDP samples (`nic_<host>.log`, moved to `<run>/` at stop). The receiver watchdog also checks it: whenever the run's receiver is up and the sampler isn't (its `nicmon_<host>.pid` is gone or dead), it starts the sampler again. It doesn't touch it while no receiver is up, so a stop isn't undone. Tested: sampler stopped by hand, back 4 s later.

## The pages watchdog

`e2sar_live_pipeline.sh start` starts the new pages right after `stop` killed the old ones; once the viewer failed with `OSError: [Errno 98] Address already in use` because the old one still held :8765. The pages mode restarts the run's viewer or monitor page when its process is gone and its port is free, but only after the pipeline has started it once (its log file exists), so it never races `start`.

## Hourly rotation

`hourly.sh` starts a run if none is up, then every `EVERY_S` (3600) s:

1. `e2sar_live_pipeline.sh stop`: receivers drain and write their final statistics, logs move to `<run>/`.
2. If the run got no events (no progress line with `in N evts` for N > 0, no `Events received` > 0, no reader `.dat` file): `daos cont destroy`, then delete the reader folder, the monitor file and the run's log folder. A run with any event is kept, logs included.
3. `e2sar_live_pipeline.sh start`: a fresh container, receiver, reader, monitor and pages.

This differs from `e2sar_live_pipeline.sh watch`/`recycle`, which replace only a run with no events, keep its logs, and need SSH from the laptop.

## Start, check, stop

```bash
# on ebpf2201 (receiver and reader both here)
D=~/e2sar_run/perlmutter; cd ~/daq/e2sar-daos
setsid nohup $D/hourly.sh > /dev/null 2>&1 < /dev/null &            # starts a run, then rotates hourly
setsid nohup $D/keepalive.sh recv 129.57.178.85 > /dev/null 2>&1 < /dev/null &
setsid nohup $D/keepalive.sh pages > /dev/null 2>&1 < /dev/null &

cat $D/current; tail $D/hourly.log $D/keepalive_ebpf2201.log         # check
pgrep -fa "hourly.sh|keepalive.sh|e2sar_daos_(recv|read|monitor)|serve.py"

pkill -f hourly.sh; pkill -f keepalive.sh                            # stop the helpers first,
# then the run, from the laptop or here with PATH=$D/bin:$PATH and the same settings as hourly.sh:
LABEL=e2sar_perlmutter_lb682 LOGDIR=e2sar_run/perlmutter RECV="ebpf2201:129.57.178.85" READER=ebpf2201 \
  ENV_FILE=e2sar-instance.env scripts/e2sar_live_pipeline.sh stop
```

Stop the helpers before stopping or restarting a run by hand. Start the receiver watchdog only after `start` has finished.

## Tests (2026-11-09)

- NIC sampler stopped by hand: restarted 4 s later.
- Receiver killed with `kill -9`: back 6 s later, joined to the same run as the next writer slot.
- One rotation with `EVERY_S=30` on an empty run: the run was stopped, its container, reader folder, monitor file and log folder deleted, and a fresh run started; both watchdogs followed the new run.
- Real crashes on LB 682 (22:03–22:28 UTC): restarted each time, then the 5-minute pause after 3.

## Cautions

- **Match processes by run name.** Other sessions run receivers and pages on the same nodes; `pkill -f e2sar_daos_recv` kills theirs too. The pipeline and these helpers match `--cont-name <run>`.
- **The watchdog doesn't fix the cause of a crash.** It limits the loss to ~10 s per crash (5 min in a crash loop). On LB 682 the cause was senders sharing (event number, data id).
- **Disk.** The reader pauses below 50 GiB free (`--min-free 50`); runs with data are never deleted, so check `/scratch` on the reader node now and then.

## The scripts (as running on 2026-11-09)

`~/e2sar_run/perlmutter/keepalive.sh`:

```bash
#!/bin/bash
# Keep the current Perlmutter run (e2sar_live_pipeline.sh, LOGDIR e2sar_run/perlmutter) alive.
#   keepalive.sh recv IP   on the receiver node: when its e2sar_daos_recv dies without a clean stop
#                          (no final statistics, log not moved away), start it again with the same
#                          options, joined to the same run (a new writer slot). Only when the receiver
#                          is missing on two checks 5 s apart and its UDP port is free.
#                          It also restarts the NIC sampler (e2sar_nicmon.sh) while the receiver is up.
#   keepalive.sh pages     on the reader node: restart the run's monitor page (:8766) or viewer
#                          (:8765) if one is down.
# Follows $D/current, so it covers each new run. Log: $D/keepalive_<host>.log.
MODE=$1; IP=$2; H=$(hostname -s)
D=$HOME/e2sar_run/perlmutter; LOG=$D/recv_$H.log; KL=$D/keepalive_$H.log
source ~/.bashrc >/dev/null 2>&1; source ~/e2sar-instance.env
cd ~/daq/e2sar-daos
say() { echo "$(date -u +%FT%TZ) $*" >> $KL; }
say "keepalive $MODE started"; n=0; miss=0; times=""
while :; do
    sleep 5
    R=$(cat $D/current 2>/dev/null); [ -n "$R" ] || continue
    if [ "$MODE" = pages ]; then
        if [ -f $D/viewer.log ] && ! pgrep -f "[s]erve.py --live /scratch/xmei/live_$R " >/dev/null && ! ss -ltn "sport = :8765" | grep -q 8765; then
            say "$R: viewer down, starting it"
            (cd ~/daq/evio-interactive-view && setsid nohup .venv/bin/python serve.py --live /scratch/xmei/live_$R --port 8765 --no-browser >> $D/viewer.log 2>&1 < /dev/null &)
        fi
        if [ -f $D/monitor_serve.log ] && ! pgrep -f "[s]erve.py /scratch/xmei/${R}_monitor.jsonl" >/dev/null && ! ss -ltn "sport = :8766" | grep -q 8766; then
            say "$R: monitor page down, starting it"
            (setsid nohup python3 viz/daos_monitor/serve.py /scratch/xmei/${R}_monitor.jsonl --port 8766 >> $D/monitor_serve.log 2>&1 < /dev/null &)
        fi
        continue
    fi
    up() { pgrep -f "[e]2sar_daos_recv .*--cont-name $R( |\$)" >/dev/null || ss -uln "sport = :20700" | grep -q 20700; }
    if up; then
        miss=0
        # NIC sampler: on whenever the run's receiver is (start turns it on, stop turns it off)
        P=$D/nicmon_$H.pid
        if ! { [ -s $P ] && kill -0 "$(cat $P)" 2>/dev/null; }; then
            say "$R: NIC monitor not running, starting it"
            bash scripts/e2sar_nicmon.sh start $D >> $KL 2>&1
        fi
        continue
    fi
    miss=$((miss+1)); [ $miss -ge 2 ] || continue; miss=0
    [ "$(cat $D/current 2>/dev/null)" = "$R" ] || continue
    [ -f $LOG ] || continue                                                  # stopped: log moved away
    tail -5 $LOG | grep -q "Final watermark and statistics" && continue      # stopped cleanly
    # crash loop: after 3 restarts within 5 min, wait 5 min before the next one
    now=$(date +%s); recent=$(printf '%s\n' $times | awk -v t=$now '$1 > t-300' | wc -l)
    if [ $recent -ge 3 ]; then say "$R: $recent restarts in 5 min: crash loop, waiting 5 min"; sleep 300; times=""; continue; fi
    times="$times $now"
    n=$((n+1)); say "$R: receiver died, restart $n (last line: $(tail -1 $LOG | cut -c1-100))"
    echo "$(date +%s.%3N) [keepalive] receiver died; restart $n" >> $LOG
    setsid nohup ./build/bin/e2sar_daos_recv -c --ip $IP --port 20700 --pool sc26 --cont-name $R \
        --threads 4 --deq 16 --report 10 --log-idle -b 1073741824 >> $LOG 2>&1 < /dev/null &
    sleep 10
done
```

`~/e2sar_run/perlmutter/hourly.sh`:

```bash
#!/bin/bash
# Perlmutter live run on LB 682, everything on ebpf2201 (receiver, reader, monitor, pages).
# Starts a run if none is up; then every EVERY_S seconds stops the current run and starts a fresh one.
# A stopped run that got no events is deleted: its container, reader folder, monitor file and logs.
# A run with any event is kept. Log: $D/hourly.log.
D=$HOME/e2sar_run/perlmutter; HL=$D/hourly.log; EVERY_S=${EVERY_S:-3600}
export PATH=$D/bin:$PATH LABEL=e2sar_perlmutter_lb682 LOGDIR=e2sar_run/perlmutter OUT=/scratch/xmei \
       RECV="ebpf2201:129.57.178.85" READER=ebpf2201 ENV_FILE=e2sar-instance.env MON_PORT=8766 VIEW_PORT=8765
source ~/.bashrc >/dev/null 2>&1
cd ~/daq/e2sar-daos; P=scripts/e2sar_live_pipeline.sh
say() { echo "[$(date -u +%FT%TZ)] $*" >> $HL; }
has_events() {   # $1 = stopped run: any event received, or any reader file
    grep -qhE " in [1-9][0-9]* evts|Events received: +[1-9]" $D/$1/recv_*.log 2>/dev/null && return 0
    ls $OUT/live_$1/*.dat >/dev/null 2>&1
}
say "hourly started (every $EVERY_S s)"
bash $P start >> $HL 2>&1 || say "start refused (a run is up?): keeping it"
while :; do
    sleep $EVERY_S
    R=$(cat $D/current 2>/dev/null)
    say "rotating $R"
    bash $P stop >> $HL 2>&1
    if [ -n "$R" ] && ! has_events $R; then
        say "$R got no events: deleting it and its logs"
        daos cont destroy sc26 $R >> $HL 2>&1
        rm -rf $OUT/live_$R $OUT/${R}_monitor.jsonl $D/$R
    else
        say "$R has data: kept (logs in $D/$R)"
    fi
    bash $P start >> $HL 2>&1
done
```

`~/e2sar_run/perlmutter/bin/ssh`:

```bash
#!/bin/bash
# Stand-in for ssh so e2sar_live_pipeline.sh can run on ebpf2201 itself (it can't ssh to itself):
# "ssh -x ebpf2201 bash -s -- ARGS" runs "bash -s -- ARGS" here.
while [[ $1 == -* ]]; do shift; done
[ "$1" = ebpf2201 ] || { echo "local ssh stand-in: only ebpf2201, not $1" >&2; exit 255; }
shift; exec "$@"
```
