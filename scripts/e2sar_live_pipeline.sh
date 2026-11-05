#!/bin/bash
# e2sar_live_pipeline.sh: run the live streaming pipeline of docs/concepts/streaming-workflow.md on the
# ebpf nodes for a remote sender through an LB, and keep it ready for data: a run that gets no events
# for IDLE_S seconds (no sender yet) is thrown away and replaced by a fresh one, so the run that does get
# data starts close to when the data does. Drive it from a machine that can SSH to every node (the nodes
# can't SSH to each other).
#
#   e2sar_live_pipeline.sh start     a new run, container LABEL_<UTC epoch s>: on each receiver node the
#                                    NIC monitor, then e2sar_daos_recv -c (the LB in EJFAT_URI from
#                                    ENV_FILE); on the reader node e2sar_daos_monitor and its page,
#                                    e2sar_daos_read --live, and the EVIO viewer. Refuses if a run is up
#   e2sar_live_pipeline.sh stop      stop the current run: receivers (Ctrl-C: they deregister and drain),
#                                    reader, monitor, pages, NIC monitors. Its data stays; its logs move
#                                    to LOGDIR/<run>/
#   e2sar_live_pipeline.sh status    what runs where, the LB's senders and workers, events per receiver,
#                                    reader files, missing events, the NIC report per node, free space,
#                                    and whether the reader is paused for space (READ_ARGS --min-free)
#   e2sar_live_pipeline.sh recycle   if the current run is at least IDLE_S old and has no event at all
#                                    (every receiver log says "in 0 evts" and the reader wrote no file):
#                                    stop it, delete it (container, empty reader folder, monitor file;
#                                    logs kept in LOGDIR/empty_<run>/) and start a new one. A run with any
#                                    event, or one it can't check, is never deleted
#   e2sar_live_pipeline.sh watch     status, then recycle, every EVERY_S seconds, until Ctrl-C. Warns when
#                                    the reader node has less than MIN_FREE_GB free
#
# The current run's name is in LOGDIR/current. Paths below are relative to the nodes' $HOME (shared
# between the ebpf nodes), except OUT. Never frees the LB.
#
# Settings (environment):
: "${RECV:=ebpf2201:129.57.178.85 ebpf2203:129.57.178.86}"   # receiver nodes as host:data IP, in start order
: "${READER:=ebpf2203}"                                       # reader, monitor and viewer node
: "${LABEL:=e2sar_live}"                                      # container = LABEL_<UTC epoch s>
: "${POOL:=sc26}"
: "${PORT:=20700}"                                            # first UDP port of each receiver
: "${RECV_ARGS:=--threads 4 --deq 16 --report 10 --log-idle}"
: "${READ_ARGS:=--min-free 50}"                               # e2sar_daos_read --live: pauses below 50 GiB free in OUT
: "${OUT:=/scratch/$USER}"                                    # reader files: OUT/live_<run>; monitor: OUT/<run>_monitor.jsonl
: "${LOGDIR:=e2sar_run/live}"
: "${REPO:=daq/e2sar-daos}"                                   # built e2sar-daos checkout
: "${VIEWER:=daq/evio-interactive-view}"                      # evio-interactive-view checkout (with .venv)
: "${ENV_FILE:=e2sar-instance.env}"                           # export EJFAT_URI='...' (lb-test 3.3)
: "${MON_PORT:=8766}"                                         # DAOS monitor page
: "${VIEW_PORT:=8765}"                                        # EVIO viewer
: "${IDLE_S:=3600}"
: "${EVERY_S:=3600}"
: "${MIN_FREE_GB:=150}"
#
# Example, LB 677 on 2026-11-05 (pages forwarded with ssh -L 8765:localhost:8765 -L 8767:localhost:8767):
#   LABEL=e2sar_lb677 OUT=/scratch/epsdaq LOGDIR=e2sar_run/lb677 MON_PORT=8767 e2sar_live_pipeline.sh start
#   ... the same settings ... e2sar_live_pipeline.sh watch

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
die() { echo "ERROR: $*" >&2; exit 1; }
say() { echo "[$(date -u +%T) UTC] $*"; }

# on HOST [ARGS...] <<'EOF' ... EOF: run a bash script on HOST with the settings exported. The script
# comes on stdin (the login shell may be tcsh: no redirections in the ssh command line).
on() {
    local h=$1; shift
    { printf 'export POOL=%q PORT=%q RECV_ARGS=%q READ_ARGS=%q OUT=%q LOGDIR=%q REPO=%q VIEWER=%q ENV_FILE=%q MON_PORT=%q VIEW_PORT=%q\n' \
          "$POOL" "$PORT" "$RECV_ARGS" "$READ_ARGS" "$OUT" "$LOGDIR" "$REPO" "$VIEWER" "$ENV_FILE" "$MON_PORT" "$VIEW_PORT"
      echo 'source ~/.bashrc >/dev/null 2>&1; L=$HOME/$LOGDIR; H=$(hostname -s); mkdir -p "$L"'
      cat; } | ssh -x "$h" bash -s -- "$@"
}

# reachable: every node answers over SSH (an SSH failure must never look like "no run")
reachable() {
    local r
    for r in $RECV "$READER"; do
        [ "$(on "${r%%:*}" <<<'echo ok' 2>/dev/null)" = ok ] || { echo "${r%%:*}"; return 1; }
    done
}

# (Remote snippets used inside $(...) are passed as strings: bash 3.2, as on macOS, can't parse a
# here-document inside a command substitution reliably.)
current() { on "$READER" <<<'cat "$L/current" 2>/dev/null'; }

# running RUN: how many of its receivers are up
running() {
    local n=0 r k
    for r in $RECV; do
        k=$(on "${r%%:*}" "$1" <<<'pgrep -fc "[e]2sar_daos_recv .*--cont-name $1( |\$)" || true')
        n=$((n + ${k:-0}))
    done
    echo $n
}

cmd_start() {
    local c r first
    c=$(current)
    [ -n "$c" ] && [ "$(running "$c")" -gt 0 ] && die "run $c is running: stop it first"
    RUN=${LABEL}_$(date -u +%s)
    on "$READER" "$RUN" <<'EOF'
echo "$1" > "$L/current"
EOF
    say "start $RUN"
    for r in $RECV; do
        on "${r%%:*}" "$RUN" "${r#*:}" <<'EOF' || die "receiver on ${r%%:*} didn't start"
source "$HOME/$ENV_FILE" || exit 1
cd "$HOME/$REPO" || exit 1
bash scripts/e2sar_nicmon.sh start "$L"
setsid nohup ./build/bin/e2sar_daos_recv -c --ip "$2" --port "$PORT" --pool "$POOL" --cont-name "$1" $RECV_ARGS \
    > "$L/recv_$H.log" 2>&1 < /dev/null &
# wait until it listens: the first receiver also creates the container the next one joins
for i in $(seq 1 60); do grep -q 'Receiving;' "$L/recv_$H.log" && break; kill -0 $! 2>/dev/null || break; sleep 1; done
grep -q 'Receiving;' "$L/recv_$H.log" || { tail -5 "$L/recv_$H.log"; exit 1; }
echo "[$H] e2sar_daos_recv listening on $2:$PORT"
EOF
    done
    on "$READER" "$RUN" <<'EOF' || die "reader side didn't start on $READER"
source "$HOME/$ENV_FILE"
D=$OUT/live_$1; mkdir -p "$D"
cd "$HOME/$REPO" || exit 1
setsid nohup ./build/bin/e2sar_daos_monitor --pool "$POOL" --cont "$1" -o "$OUT/$1_monitor.jsonl" > "$L/monitor.log" 2>&1 < /dev/null &
sleep 2
setsid nohup python3 viz/daos_monitor/serve.py "$OUT/$1_monitor.jsonl" --port "$MON_PORT" > "$L/monitor_serve.log" 2>&1 < /dev/null &
setsid nohup ./build/bin/e2sar_daos_read --pool "$POOL" --cont "$1" -o "$D" --live $READ_ARGS > "$L/read.log" 2>&1 < /dev/null &
cd "$HOME/$VIEWER" || exit 1
setsid nohup .venv/bin/python serve.py --live "$D" --port "$VIEW_PORT" --no-browser > "$L/viewer.log" 2>&1 < /dev/null &
sleep 4
grep -q 'writer slot' "$L/read.log" || { tail -3 "$L/read.log"; exit 1; }
echo "[$H] reader -> $D, monitor page :$MON_PORT, viewer :$VIEW_PORT"
"$E2SAR_BIN/lbadm" --status 2>&1 | grep -oE 'name=[^,]+|slotsassigned=[0-9]+' | paste - - | sed "s/^/[LB] /"
EOF
    say "running $RUN"
}

cmd_stop() {
    local c r
    c=$(current); [ -n "$c" ] || die "no current run in $LOGDIR/current"
    say "stop $c"
    for r in $RECV; do
        on "${r%%:*}" "$c" <<'EOF'
pkill -INT -f "[e]2sar_daos_recv .*--cont-name $1( |\$)"
for i in $(seq 1 60); do pgrep -f "[e]2sar_daos_recv .*--cont-name $1( |\$)" >/dev/null || break; sleep 1; done
pgrep -f "[e]2sar_daos_recv .*--cont-name $1( |\$)" >/dev/null && echo "[$H] receiver still running after 60 s"
bash "$HOME/$REPO/scripts/e2sar_nicmon.sh" stop "$L"
mkdir -p "$L/$1"
mv "$L/recv_$H.log" "$L/nic_$H.log" "$L/nic_before_$H.txt" "$L/nic_after_$H.txt" "$L/$1/" 2>/dev/null
grep -E 'Events (reassembled|received|committed|failed)' "$L/$1/recv_$H.log" | sed "s/^[0-9.]* \t*/[$H] /"
EOF
    done
    on "$READER" "$c" <<'EOF'
# the reader ends by itself once every writer is done (--end-wait 10 s)
for i in $(seq 1 60); do pgrep -f "[e]2sar_daos_read .*--cont $1( |\$)" >/dev/null || break; sleep 1; done
pkill -f "[e]2sar_daos_read .*--cont $1( |\$)"
# the monitor stops on Ctrl-C; KILL if it doesn't
pkill -INT -f "[e]2sar_daos_monitor .*--cont $1( |\$)"; sleep 3
pkill -KILL -f "[e]2sar_daos_monitor .*--cont $1( |\$)"
pkill -f "[s]erve.py .*$1"
mkdir -p "$L/$1"
mv "$L/read.log" "$L/monitor.log" "$L/monitor_serve.log" "$L/viewer.log" "$L/$1/" 2>/dev/null
grep -E 'end of run' "$L/$1/read.log" | sed "s/^[0-9.]* /[$H] reader: /"
EOF
    say "stopped $c (logs in ~/$LOGDIR/$c/)"
}

cmd_status() {
    local c r
    c=$(current); [ -n "$c" ] || { say "no current run"; return 0; }
    say "run $c, $(( $(date -u +%s) - ${c##*_} )) s old"
    for r in $RECV; do
        on "${r%%:*}" "$c" <<'EOF'
up=$(pgrep -fc "[e]2sar_daos_recv .*--cont-name $1( |\$)" || true)
last=$(grep -E ' in [0-9]+ evts' "$L/recv_$H.log" 2>/dev/null | tail -1)
loss=$(echo "$last" | grep -oE 'reassembly loss [0-9]+, enqueue loss [0-9]+')
echo "[$H] receiver up: $up; $(echo "$last" | grep -oE 'in [0-9]+ evts, [0-9.]+ Gbps'); ${loss:-no progress line yet}"
bash "$HOME/$REPO/scripts/e2sar_nicmon.sh" report "$L" 2>&1 | tail -4 | sed "s/^/[$H] nic: /"
EOF
    done
    on "$READER" "$c" <<'EOF'
source "$HOME/$ENV_FILE" 2>/dev/null
D=$OUT/live_$1
echo "[$H] reader $(pgrep -fc "[e]2sar_daos_read .*--cont $1( |\$)" || true), monitor $(pgrep -fc "[e]2sar_daos_monitor .*--cont $1( |\$)" || true), pages $(pgrep -fc "[s]erve.py .*$1" || true); files: $(ls "$D"/*.evio 2>/dev/null | wc -l) in $D"
tail -1 "$L/monitor.log" 2>/dev/null | grep -oE 'in DAOS [0-9]+ events.*' | sed "s/^/[$H] monitor: /"
"$E2SAR_BIN/lbadm" --status 2>&1 | grep -E 'Registered senders' | sed "s/^/[LB] /"
"$E2SAR_BIN/lbadm" --status 2>&1 | grep -oE 'name=[^,]+|slotsassigned=[0-9]+|totalEventsRecv=[0-9]+' | paste - - - | sed "s/^/[LB] /"
echo "[$H] free on $(df -P "$OUT" | awk 'NR==2{print $6": "int($4/1048576)" GB"}')"
p=$(grep -E ' (pausing|still paused|resuming): ' "$L/read.log" 2>/dev/null | tail -1)
case "$p" in *paus*) echo "[$H] READER PAUSED (--min-free): ${p#* }" ;; esac
EOF
}

# Remote snippets whose output is captured (see the note above current)
read -r -d '' EVENTS_SNIP <<'EOF'
# the receiver's events so far, from its last progress line; "unknown" if its log isn't this run's
grep -q "^[0-9.]* Writer slot" "$L/recv_$H.log" 2>/dev/null || { echo unknown; exit; }
n=$(grep -oE ' in [0-9]+ evts' "$L/recv_$H.log" | tail -1 | grep -oE '[0-9]+')
echo "${n:-unknown}"
EOF
read -r -d '' FILES_SNIP <<'EOF'
ls "$OUT/live_$1"/*.evio 2>/dev/null | wc -l | tr -d ' '
EOF
read -r -d '' COMMITTED_SNIP <<'EOF'
n=$(grep -oE 'Events committed: +[0-9]+' "$L/$1/recv_$H.log" | grep -oE '[0-9]+$')
echo "${n:-unknown}"
EOF
read -r -d '' FREE_SNIP <<'EOF'
df -P "$OUT" | awk 'NR==2{print int($4/1048576)}'
EOF

# empty RUN: prints "yes" only if every receiver log shows 0 events and the reader wrote no file
empty() {
    local r n
    for r in $RECV; do
        n=$(on "${r%%:*}" "$1" <<<"$EVENTS_SNIP")
        [ "$n" = 0 ] || { echo "no (${r%%:*}: ${n:-unknown})"; return; }
    done
    n=$(on "$READER" "$1" <<<"$FILES_SNIP")
    [ "$n" = 0 ] && echo yes || echo "no (reader files: ${n:-unknown})"
}

cmd_recycle() {
    local c age e
    c=$(current); [ -n "$c" ] || { say "no current run: start one"; cmd_start; return; }
    age=$(( $(date -u +%s) - ${c##*_} ))
    [ "$age" -ge "$IDLE_S" ] || { say "keep $c: $age s old (< IDLE_S=$IDLE_S)"; return 0; }
    e=$(empty "$c")
    [ "$e" = yes ] || { say "keep $c: it has data or can't be checked ($e)"; return 0; }
    say "$c: no events in $age s (no sender): recycle"
    cmd_stop
    # check again from the final statistics before deleting anything
    for r in $RECV; do
        e=$(on "${r%%:*}" "$c" <<<"$COMMITTED_SNIP")
        [ "$e" = 0 ] || { say "keep $c: ${r%%:*} committed ${e:-unknown} events after all"; return 0; }
    done
    on "$READER" "$c" <<'EOF'
daos cont destroy "$POOL" "$1" | tail -1
rmdir "$OUT/live_$1" 2>/dev/null; rm -f "$OUT/$1_monitor.jsonl"
rm -f "$L/$1"/nic_*
mv "$L/$1" "$L/empty_$1"
echo "[$H] deleted $1; logs in ~/$LOGDIR/empty_$1/"
EOF
    cmd_start
}

cmd_watch() {
    local free
    while :; do
        if ! bad=$(reachable); then
            say "WARNING: can't reach $bad over SSH; nothing checked, nothing changed"
            sleep "$EVERY_S"; continue
        fi
        cmd_status
        cmd_recycle
        free=$(on "$READER" <<<"$FREE_SNIP")
        [ "${free:-0}" -ge "$MIN_FREE_GB" ] || say "WARNING: $READER has ${free:-?} GB free in $OUT (< $MIN_FREE_GB GB)"
        sleep "$EVERY_S"
    done
}

case "${1:-}" in
    start|stop|status|recycle) bad=$(reachable) || die "can't reach $bad over SSH" ;;
esac
case "${1:-}" in
    start) cmd_start ;;
    stop) cmd_stop ;;
    status) cmd_status ;;
    recycle) cmd_recycle ;;
    watch) cmd_watch ;;
    *) usage ;;
esac
