#!/bin/bash
# e2sar_workflow.sh: the live workflow of docs/howto/streaming-walkthrough.md, run node by node: log in to
# each ebpf node in turn and run the same command there. Each node starts or stops only its own part,
# so the nodes never SSH to each other; they find the run through files in the shared $HOME.
#
#   every node:      NIC monitor, then e2sar_daos_recv -c through the LB (a writer slot of the run)
#   READER node:     also the --live-rm reader, the DAOS monitor and its page, and the EVIO live viewer
#
#   e2sar_workflow.sh start            first node: check the LB instance URI and open a new run,
#                                      e2sar_lb<LB id>_<UTC epoch s>; any later node: join that run
#   e2sar_workflow.sh status           the run, every node's receiver, LB workers; on READER the bundles
#   e2sar_workflow.sh forward          the laptop command that forwards the two pages of READER
#   e2sar_workflow.sh list             (READER) the run's bundles (.dat files) with their numbers
#   e2sar_workflow.sh evio N [OUT]     (READER) bundle N of the list as an EVIO file (default OUT/<bundle>.evio)
#   e2sar_workflow.sh evio FILE.dat [OUT]   the same for a bundle named by its file
#   e2sar_workflow.sh evio all [OUT]   (READER) the whole run as one EVIO file (default OUT/<run>.evio)
#   e2sar_workflow.sh stop             this node's part: receiver (drains), NIC monitor; on READER then the
#                                      reader (it ends once every receiver is done), monitor and pages.
#                                      Stop the other nodes first; READER refuses while one still runs
#                                      (stop force: stop the reader anyway)
#
# Order: start ebpf2201, then ebpf2203 (READER); stop ebpf2201, then ebpf2203. Logs: ~/LOGDIR/, moved
# to ~/LOGDIR/<run>/ by stop. A bundle is EVIO v6 records without the EVIO file header;
# scripts/evio/evio_concat.py adds it. evio works during the run and after stop. One workflow per LB at
# a time: two runs registered with the same LB would split its events.
#
# Settings (environment):
: "${ENV_FILE:=e2sar-lb686.env}"                              # LB instance URI file in $HOME: export EJFAT_URI='...'
: "${READER:=ebpf2203}"                                       # reader, monitor and viewer node
: "${POOL:=sc-demo}"
: "${PORT:=20700}"                                            # first UDP port of each receiver
: "${RECV_ARGS:=--threads 4 --deq 16 --report 10 --log-idle -b 1073741824}"   # keep -b 1 GiB: 3 MB drops ~10% at 1.5 Gbps
: "${READ_ARGS:=--live-rm --interval 2 --rm-after 2 --min-free 50}"
: "${OUT:=/scratch/epsdaq}"                                   # bundles: OUT/live_<run>/; monitor: OUT/<run>_monitor.jsonl
: "${LOGDIR:=e2sar_run/workflow}"                             # shared $HOME: LOGDIR/current names the run
: "${REPO:=daq/e2sar-daos}"
: "${VIEWER:=daq/evio-interactive-view}"
: "${MON_PORT:=8766}"
: "${VIEW_PORT:=8765}"

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
die() { echo "ERROR: $*" >&2; exit 1; }
say() { echo "[$(date -u +%T) UTC $H] $*"; }

source ~/.bashrc >/dev/null 2>&1 || true                       # E2SAR_BIN, MY_IP (this node's data address)
H=$(hostname -s); L=$HOME/$LOGDIR; mkdir -p "$L"
cd "$HOME/$REPO" || die "no checkout in ~/$REPO"
RUN=$(cat "$L/current" 2>/dev/null)

# receiver_up [RUN]: this node's receiver of the run is running
receiver_up() { pgrep -f "[e]2sar_daos_recv .*--cont-name ${1:-$RUN}( |\$)" >/dev/null; }
# open_nodes: other nodes whose receiver log of the current run is still in $L without final statistics
open_nodes() {
    local f
    for f in "$L"/recv_*.log; do
        [ -f "$f" ] && [ "$f" != "$L/recv_$H.log" ] || continue
        grep -q "Final watermark and statistics" "$f" || basename "$f" .log | sed 's/^recv_//'
    done
}

cmd_start() {
    [ -n "${MY_IP:-}" ] || die "MY_IP is empty: ~/.bashrc should set it"
    source "$HOME/$ENV_FILE" >/dev/null 2>&1 && [ -n "${EJFAT_URI:-}" ] || die "no EJFAT_URI in ~/$ENV_FILE"
    export EJFAT_URI
    "$E2SAR_BIN/lbadm" --status >/dev/null 2>&1 ||
        die "lbadm --status fails on EJFAT_URI from ~/$ENV_FILE: not a valid LB instance (placeholder, freed or expired)"
    local lbid; lbid=$(echo "$EJFAT_URI" | grep -oE '/lb/[0-9]+' | cut -d/ -f3)

    if [ -n "$RUN" ] && receiver_up; then die "run $RUN is already running here: stop it first"; fi
    local others; others=$(open_nodes)
    if [ -n "$RUN" ] && [ -n "$others" ]; then
        say "joining run $RUN (receiver up on: $(echo $others))"
    else
        RUN=e2sar_lb${lbid}_$(date -u +%s); echo "$RUN" > "$L/current"
        rm -f "$L"/recv_*.log
        say "new run $RUN (LB $lbid, pool $POOL)"
    fi

    bash scripts/e2sar_nicmon.sh start "$L"
    setsid nohup ./build/bin/e2sar_daos_recv -c --ip "$MY_IP" --port "$PORT" --pool "$POOL" --cont-name "$RUN" \
        $RECV_ARGS > "$L/recv_$H.log" 2>&1 < /dev/null &
    local i
    for i in $(seq 1 60); do grep -q 'Receiving;' "$L/recv_$H.log" && break; kill -0 $! 2>/dev/null || break; sleep 1; done
    grep -q 'Receiving;' "$L/recv_$H.log" || { tail -5 "$L/recv_$H.log"; die "receiver didn't start"; }
    say "e2sar_daos_recv listening on $MY_IP:$PORT"

    if [ "$H" = "$READER" ]; then
        local D=$OUT/live_$RUN; mkdir -p "$D"
        setsid nohup ./build/bin/e2sar_daos_monitor --pool "$POOL" --cont "$RUN" -o "$OUT/${RUN}_monitor.jsonl" \
            > "$L/monitor.log" 2>&1 < /dev/null &
        sleep 2
        setsid nohup python3 viz/daos_monitor/serve.py "$OUT/${RUN}_monitor.jsonl" --port "$MON_PORT" \
            > "$L/monitor_serve.log" 2>&1 < /dev/null &
        setsid nohup ./build/bin/e2sar_daos_read --pool "$POOL" --cont "$RUN" -o "$D" $READ_ARGS \
            > "$L/read.log" 2>&1 < /dev/null &
        local py=python3; [ -x "$HOME/$VIEWER/.venv/bin/python" ] && py=.venv/bin/python
        (cd "$HOME/$VIEWER" && exec setsid nohup $py serve.py --live "$D" --port "$VIEW_PORT" --no-browser) \
            > "$L/viewer.log" 2>&1 < /dev/null &
        sleep 4
        grep -q 'writer slot' "$L/read.log" || { tail -3 "$L/read.log"; die "reader didn't start"; }
        say "reader -> $D, monitor page :$MON_PORT, viewer :$VIEW_PORT"
        echo; cmd_forward
    else
        say "next: log in to $READER and run $(basename "$0") start there (reader, monitor, pages)"
    fi
    "$E2SAR_BIN/lbadm" --status 2>&1 | grep -oE 'name=[^,]+|slotsassigned=[0-9]+' | paste - - | sed 's/^/[LB] /'
}

cmd_stop() {
    [ -n "$RUN" ] || die "no run in $L/current"
    local others; others=$(open_nodes)
    if [ "$H" = "$READER" ] && [ -n "$others" ] && [ "${1:-}" != force ]; then
        die "receiver still running on: $(echo $others). Stop it there first (the reader ends once every receiver is done), or: $(basename "$0") stop force"
    fi
    say "stop $RUN"
    if receiver_up; then
        pkill -INT -f "[e]2sar_daos_recv .*--cont-name $RUN( |\$)"
        local i; for i in $(seq 1 60); do receiver_up || break; sleep 1; done
        receiver_up && say "receiver still running after 60 s"
    fi
    bash scripts/e2sar_nicmon.sh stop "$L"
    mkdir -p "$L/$RUN"
    mv "$L/recv_$H.log" "$L/nic_$H.log" "$L/nic_before_$H.txt" "$L/nic_after_$H.txt" "$L/$RUN/" 2>/dev/null
    grep -E 'Events (reassembled|received|committed|failed)' "$L/$RUN/recv_$H.log" 2>/dev/null | sed "s/^[0-9.]* \t*/[$H] /"
    [ "$H" = "$READER" ] || { say "next: log in to $READER and run $(basename "$0") stop there"; return; }

    for i in $(seq 1 60); do pgrep -f "[e]2sar_daos_read .*--cont $RUN( |\$)" >/dev/null || break; sleep 1; done
    pkill -f "[e]2sar_daos_read .*--cont $RUN( |\$)"
    pkill -INT -f "[e]2sar_daos_monitor .*--cont $RUN( |\$)"; sleep 3
    pkill -KILL -f "[e]2sar_daos_monitor .*--cont $RUN( |\$)"
    pkill -f "[s]erve.py .*$RUN"
    mv "$L/read.log" "$L/monitor.log" "$L/monitor_serve.log" "$L/viewer.log" "$L/$RUN/" 2>/dev/null
    grep -E 'end of run' "$L/$RUN/read.log" | sed "s/^[0-9.]* /[$H] reader: /"
    say "stopped $RUN (logs in ~/$LOGDIR/$RUN/, bundles in $OUT/live_$RUN/)"
}

cmd_status() {
    [ -n "$RUN" ] || die "no run in $L/current"
    echo "run $RUN"
    local f h
    for f in "$L"/recv_*.log "$L/$RUN"/recv_*.log; do
        [ -f "$f" ] || continue; h=$(basename "$f" .log | sed 's/^recv_//')
        if grep -q "Final watermark and statistics" "$f"; then echo "  $h: receiver stopped"
        else echo "  $h: receiver running: $(grep -E ' in [0-9]+ evts' "$f" | tail -1 | cut -c1-140)"; fi
    done
    "$E2SAR_BIN/lbadm" --status 2>&1 | grep -oE 'name=[^,]+|slotsassigned=[0-9]+' | paste - - | sed 's/^/  [LB] /'
    if [ "$H" = "$READER" ]; then
        echo "  here: $(pgrep -fc "[e]2sar_daos_read .*--cont $RUN( |\$)") reader, $(pgrep -fc "[e]2sar_daos_monitor .*--cont $RUN( |\$)") monitor, $(pgrep -fc "[s]erve.py .*$RUN") pages"
        echo "  bundles: $(ls "$OUT/live_$RUN"/*.dat 2>/dev/null | wc -l) in $OUT/live_$RUN; $(df -h "$OUT" | awk 'NR==2{print $4}') free"
        grep -v rsvc_client "$L/read.log" 2>/dev/null | tail -1 | sed 's/^/  reader: /'
    fi
}

cmd_forward() {
    cat <<EOF
On the laptop, forward the two pages of $READER (one SSH connection; add -J scilogin.jlab.org,tblogin.jlab.org
from outside, or use a Host $READER entry with ProxyJump):
  ssh -N -L $MON_PORT:localhost:$MON_PORT -L $VIEW_PORT:localhost:$VIEW_PORT $READER
or, with a ControlMaster connection to $READER already open:
  ssh -O forward -L $MON_PORT:localhost:$MON_PORT -L $VIEW_PORT:localhost:$VIEW_PORT $READER
Then open http://localhost:$MON_PORT (DAOS monitor) and http://localhost:$VIEW_PORT (EVIO live viewer).
EOF
}

# manifest: the run's bundle list on this node, one line per .dat file (file in column 2, events in column 5)
manifest() {
    [ "$H" = "$READER" ] || die "the bundles are on $READER's /scratch: run this there"
    [ -n "$RUN" ] || die "no run in $L/current"
    D=$OUT/live_$RUN; M=$D/${RUN}_manifest.txt
    [ -s "$M" ] || die "no bundles yet in $D"
}

cmd_list() {
    manifest
    echo "$RUN: $(wc -l < "$M") bundles in $D"
    awk '{printf "%5d  %s  %8d events  %8.1f MiB\n", NR, $2, $5, $6 / 1048576}' "$M"
}

cmd_evio() {
    local sel=${1:-} out=${2:-} files f
    [ -n "$sel" ] || { cmd_list; echo; echo "Pick one: $(basename "$0") evio N | FILE.dat | all [OUT]"; exit 1; }
    manifest
    case $sel in
        all)      files=$(awk '{print $2}' "$M"); out=${out:-$OUT/$RUN.evio} ;;
        *.dat)    f=$(basename "$sel"); grep -q " $f " "$M" || die "$f is not in $M"
                  files=$f; out=${out:-$OUT/${f%.dat}.evio} ;;
        *[!0-9]*) die "evio takes a bundle number, a .dat file or 'all', not '$sel'" ;;
        *)        f=$(awk -v n="$sel" 'NR == n {print $2}' "$M"); [ -n "$f" ] || die "no bundle $sel (see: list)"
                  files=$f; out=${out:-$OUT/${f%.dat}.evio} ;;
    esac
    [ -e "$out" ] && die "$out exists: remove it or give another OUT"
    (cd "$D" && python3 -I "$HOME/$REPO/scripts/evio/evio_concat.py" "$out" $files) || die "evio_concat.py failed"
    chmod g+r "$out"
    [ "$(stat -c %s "$out")" -lt 2147483648 ] ||
        echo "Note: over 2 GiB; evio-interactive-view opens files under 2 GiB (split it with its evio_split.py)"
    sha256sum "$out"
    echo "For someone in group epsci on $H: $out"
    echo "To copy it off the node, from the laptop: scp $H:$out ."
}

case "${1:-}" in
    start)   cmd_start ;;
    stop)    cmd_stop "${2:-}" ;;
    status)  cmd_status ;;
    forward) cmd_forward ;;
    list)    cmd_list ;;
    evio)    shift; cmd_evio "$@" ;;
    *)       usage ;;
esac
