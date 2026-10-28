#!/bin/bash
# e2sar_e2e_sweep.sh: a rate sweep of e2sar_e2e_bench.sh, driven from a machine that can SSH
# to both nodes (the nodes can't SSH to each other).
#
#   e2sar_e2e_sweep.sh STAMP "RATES" [REPS]
#
# For each repetition, for each rate (so the repetitions are spread over time), it starts the
# receiver on $RECV_HOST, then the sender on $SEND_HOST, with TAG sw_<mode>_r<rate>_<rep>_<STAMP>,
# and waits for the receiver's result. Results land in $RUN/bench/results.jsonl on the nodes,
# as for single runs; this script keeps a log and prints each run's result line.
#
# Settings (environment):
: "${SEND_HOST:=ebpf2201}"
: "${RECV_HOST:=ebpf2203}"
: "${MODE:=n2n}"                                            # lb | n2n
: "${RECV_OPTS:=-T 32 -D 32}"                               # e.g. "-T 32 -D 32 -S pil4dfs"
: "${SEND_OPTS:=-P 16 -s 1 -n 6250 -f 256}"                 # -r is added per run
: "${BENCH:=bash ~/daq/e2sar-daos/scripts/e2sar_e2e_bench.sh}"  # as run on the nodes
: "${LOG:=./sweep_${MODE}_${1:-}.log}"
#
# Example, the node-to-node sweep of docs/E2E_BENCHMARK.md section 7:
#   e2sar_e2e_sweep.sh 1020a "20 25 30 35 40 45 50" 3
#   RECV_OPTS="-T 32 -D 32 -S pil4dfs" e2sar_e2e_sweep.sh 1020p "20 30 40 50" 3

STAMP=${1:-}; RATES=${2:-}; REPS=${3:-3}
[ -n "$STAMP" ] && [ -n "$RATES" ] || { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

for rep in $(seq 1 "$REPS"); do
    for r in $RATES; do
        TAG=sw_${MODE}_r${r}_${rep}_${STAMP}
        echo "=== $TAG $(date +%T)" >> "$LOG"
        # -x: no X11 forwarding; no redirections in the remote command (tcsh login shells)
        ssh -x "$RECV_HOST" "$BENCH recv $TAG -m $MODE $RECV_OPTS" >> "$LOG" 2>&1 < /dev/null &
        RP=$!
        sleep 3
        ssh -x "$SEND_HOST" "$BENCH send $TAG -m $MODE $SEND_OPTS -r $r" >> "$LOG" 2>&1 < /dev/null
        wait $RP
        echo "rc=$?" >> "$LOG"
        grep "^$TAG " "$LOG" | tail -1
        sleep 5
    done
done
echo "=== done $(date +%T)" >> "$LOG"
