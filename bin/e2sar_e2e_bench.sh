#!/bin/bash
# e2sar_e2e_bench.sh: one end-to-end run of synthetic EVIO time frames into DAOS.
#
#   sender node:    e2sar_evio_send ──▶ [EJFAT LB] ──▶ e2sar_daos_recv ──▶ DAOS   :receiver node
#
# Run both roles at the same time, one on each node, with the same TAG and mode:
#   receiver:  e2sar_e2e_bench.sh recv TAG [-m lb|b2b] [-T recv_threads]
#   sender:    e2sar_e2e_bench.sh send TAG [-m lb|b2b] [-P segmenters] [-s sockets]
#                                          [-n events_per_segmenter] [-f frames_per_event] [-r gbps]
#   either:    e2sar_e2e_bench.sh clean
#
# Modes:
#   lb   through the LB in $EJFAT_URI, both sides with --withcp. The LB spreads events over the
#        receiver's -T ports by their random entropy.
#   b2b  back-to-back, no LB. Segmenter i sends to port PORT+i (--port-per-segmenter), so use
#        -T >= -P on the receiver.
#
# The nodes can't SSH to each other, so the roles meet through files in $RUN/bench/TAG/
# (an NFS $HOME shared by both nodes): recv writes "ready" once it listens (with the LB:
# once the LB has assigned it slots); send writes "sent" when e2sar_evio_send has exited.
# The receiver then waits for every event to be committed (or 15 s without progress),
# stops, reads every event back from DAOS to check its SHA-256, runs e2sar_e2e_analyze.py,
# prints the result, appends it to $RUN/bench/results.jsonl and destroys the container.
#
# Settings come from ~/.bashrc (E2SAR_BIN; EJFAT_URI = LB instance URI for -m lb) and these
# environment variables, with defaults for the ebpf testbed:
: "${SEND_IP:=129.57.178.85}"            # sender data-plane address (goes on the LB allowlist)
: "${RECV_IP:=129.57.178.86}"            # receiver listen address
: "${PORT:=20600}"                       # first receiver UDP port
: "${POOL:=sc26}"
: "${CONT:=e2sar-bench}"                 # DAOS container, destroyed after every run
: "${NIC:=enp1s0f1np1}"                  # receiver data-plane NIC, for drop counters
: "${RUN:=$HOME/e2sar_run}"              # shared: logs and rendezvous files
: "${E2SAR_DAOS_BUILD:=$(cd "$(dirname "$0")/.." && pwd)/build}"

source ~/.bashrc >/dev/null 2>&1 || true   # before set -u: .bashrc uses unset variables
set -u
: "${E2SAR_BIN:?E2SAR_BIN not set (see docs/ESNET_LB_TESTING.md step 1)}"
BIN=$E2SAR_DAOS_BUILD/bin
HERE=$(cd "$(dirname "$0")" && pwd)

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
die() { echo "ERROR: $*" >&2; exit 1; }
# wait_for FILE SECONDS: wait until FILE exists
wait_for() { local i; for ((i = 0; i < $2; i++)); do [ -e "$1" ] && return 0; sleep 1; done; return 1; }
nic_counters() { /usr/sbin/ethtool -S "$NIC" | awk '/ (rx_out_of_buffer|rx_discards_phy):/{gsub(":", "", $1); print $1, $2}'; }

ROLE=${1:-}; shift || true
if [ "$ROLE" = clean ]; then
    daos cont destroy "$POOL" "$CONT" >/dev/null 2>&1
    echo "removed $POOL/$CONT (if present)"; exit 0
fi
TAG=${1:-}; shift || true
[ -n "$TAG" ] && [[ "$ROLE" =~ ^(send|recv)$ ]] || usage

MODE=lb SEGS=4 SOCKETS=2 EVENTS=100000 FRAMES=1 RATE=-1 THREADS=32 POOLSZ=64
while getopts "m:P:s:n:f:r:T:" o; do
    case $o in
        m) MODE=$OPTARG ;; P) SEGS=$OPTARG ;; s) SOCKETS=$OPTARG ;; n) EVENTS=$OPTARG ;;
        f) FRAMES=$OPTARG ;; r) RATE=$OPTARG ;; T) THREADS=$OPTARG ;; *) usage ;;
    esac
done
case $MODE in
    lb)  : "${EJFAT_URI:?EJFAT_URI (LB instance URI) not set}"; URI=$EJFAT_URI; CP=--withcp ;;
    b2b) URI="ejfat://useless@10.10.10.10:1234/lb/1?data=$RECV_IP:$PORT"; CP= ;;
    *)   usage ;;
esac

D=$RUN/bench/$TAG
mkdir -p "$D"

# ---------------------------------------------------------------- sender
if [ "$ROLE" = send ]; then
    echo "[send $TAG] waiting for the receiver"
    wait_for "$D/ready" 180 || die "receiver not ready after 180 s ($D/ready)"
    if [ "$MODE" = lb ]; then
        ARGS=(--withcp --ip "$SEND_IP")
    else
        ARGS=(--port-per-segmenter)
    fi
    "$BIN/e2sar_evio_send" -u "$URI" "${ARGS[@]}" --mtu 9000 -P "$SEGS" -s "$SOCKETS" -n "$EVENTS" \
        -f "$FRAMES" -r "$RATE" --pool "$POOLSZ" --timing-csv "$D/send.csv" --digests "$D/digests" \
        > "$D/send.log" 2>&1
    RC=$?
    echo "rc=$RC mode=$MODE segmenters=$SEGS sockets=$SOCKETS events=$((SEGS * EVENTS)) frames=$FRAMES rate=$RATE pool=$POOLSZ" > "$D/sent"
    grep -E "Event:|Sent:|Sender goodput" "$D/send.log" | sed "s/^/[send $TAG] /"
    exit $RC
fi

# ---------------------------------------------------------------- receiver
ss -uan | grep -q ":$PORT\b" && die "UDP port $PORT is in use (ss -uanp); someone else may be testing"
daos cont destroy "$POOL" "$CONT" >/dev/null 2>&1
nic_counters > "$D/nic0"; nstat -n
"$BIN/e2sar_daos_recv" -u "$URI" $CP --ip "$RECV_IP" --port "$PORT" --threads "$THREADS" \
    --pool "$POOL" --cont "$CONT" --prefix "$TAG" --timeout 10000 -b 1073741824 \
    --deq 4 --inflight 128 --timing-csv "$D/timing.csv" > "$D/recv.log" 2>&1 < /dev/null &
PID=$!
trap 'kill -INT $PID 2>/dev/null; wait $PID' EXIT

for ((i = 0; i < 60; i++)); do
    sleep 1
    kill -0 $PID 2>/dev/null || die "receiver exited: $(tail -3 "$D/recv.log")"
    grep -q "Receiving" "$D/recv.log" || continue
    [ "$MODE" = b2b ] && break
    "$E2SAR_BIN/lbadm" --status 2>/dev/null | grep "IP Address=$RECV_IP, UDP Port=$PORT," \
        | grep -q 'slotsassigned=[1-9]' && break
done
[ $i -lt 60 ] || die "receiver not ready in 60 s (LB slots?)"
touch "$D/ready"
echo "[recv $TAG] $MODE: listening on $THREADS ports from $PORT; waiting for the sender"

wait_for "$D/sent" 3600 || die "no sender after 1 h"
eval "$(tr ' ' '\n' < "$D/sent" | sed 's/^/S_/')"   # S_events, S_pool, ...
# events can still be in reassembly (--timeout 10 s) or in DAOS puts: wait until all are
# committed, or the progress line (once a second) shows no new commits for 15 s
last=-1; idle=0
while :; do
    n=$(grep -oP 'committed \K\d+(?= evts)' "$D/recv.log" | tail -1); n=${n:-0}
    [ "$n" -ge "$S_events" ] && break
    if [ "$n" = "$last" ]; then ((idle++)); [ $idle -ge 15 ] && break; else idle=0; last=$n; fi
    sleep 1
done
sleep 2   # one more progress tick
kill -INT $PID; wait $PID; trap - EXIT

nic_counters > "$D/nic1"
DISC=$(paste -d' ' "$D/nic0" "$D/nic1" | awk '$1 == "rx_discards_phy" {print $4 - $2}')
OOB=$(paste -d' ' "$D/nic0" "$D/nic1" | awk '$1 == "rx_out_of_buffer" {print $4 - $2}')
RCVBUF=$(nstat -z UdpRcvbufErrors | awk '/UdpRcvbufErrors/{print $2}')

"$BIN/e2sar_daos_read" --pool "$POOL" --cont "$CONT" --prefix "$TAG" --fetch > "$D/fetch.txt" 2>&1
daos cont destroy "$POOL" "$CONT" >/dev/null 2>&1

GP=$(grep -oP 'Sender goodput:\s+\K[0-9.]+' "$D/send.log")
R=$(python3 -I "$HERE/e2sar_e2e_analyze.py" "$D" --pool "$S_pool" --expected "$S_events" --set \
    tag="$TAG" mode="$MODE" segmenters="$S_segmenters" sockets="$S_sockets" frames_per_event="$S_frames" \
    rate="$S_rate" recv_threads="$THREADS" sender_goodput_gbps="${GP:-0}" rx_discards_phy="$DISC" \
    rx_out_of_buffer="$OOB" udp_rcvbuf_errors="${RCVBUF:-0}" 2> "$D/summary.txt")
echo "$R" > "$D/result.json"
echo "$R" >> "$RUN/bench/results.jsonl"
cat "$D/summary.txt"
[[ "$R" == *'"result": "PASS"'* ]]
