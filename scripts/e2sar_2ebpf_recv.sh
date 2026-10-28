#!/bin/bash
# e2sar_2ebpf_recv.sh: run one of the two ebpf nodes as a receiver for docs/PERLMUTTER_GUIDE.md
# (remote sender ──▶ ESnet LB ──▶ e2sar_daos_recv on ebpf2201 and ebpf2203 ──▶ DAOS; the sender is
# on Perlmutter by default, or on Lux with SITE=lux, see below).
#
#   e2sar_2ebpf_recv.sh start          steps 4-5: pre-flight checks, run directory, start the receiver.
#                                      Run on ebpf2201 first (it makes the new RUNID), then on ebpf2203
#                                      (it waits for that RUNID). Ctrl-C stops the receiver, and the
#                                      script then does step 7's drop checks on this node.
#   e2sar_2ebpf_recv.sh status         both workers' LB slots (step 5); send only when both are > 0
#   e2sar_2ebpf_recv.sh verify N [L]   step 8, once on either node after both receivers stopped, for a
#                                      send of N events of L bytes (default 1048576). Reports the loss
#                                      rate (events that never reached DAOS) without failing on it.
#                                      FAIL only if a stored event is wrong: size, digest, an event
#                                      number >= N, or an event written twice
#   e2sar_2ebpf_recv.sh runs           the runs in the DAOS container (its run catalog), with each
#                                      run's committed and lost events
#   e2sar_2ebpf_recv.sh clean [RUNID]  step 9: delete run RUNID (its DAOS object), or without RUNID
#                                      destroy the whole container; then show the LB state
#
# Another sender site (e.g. Lux): set SITE on both nodes, for every command. Runs are then named
# lux_<date>_<time>, in container e2sar-lux and directory ~/e2sar_run/lux. Unset CONT and RUN
# first if they were exported for Perlmutter, or they override SITE.
#   unset CONT RUN; export SITE=lux
#   e2sar_2ebpf_recv.sh start          (ebpf2201 first, then ebpf2203)
#   e2sar_2ebpf_recv.sh verify 100     (status, runs and clean the same way)
#
# Every LB command uses -v (--novalidate). The LB instance URI comes from $ENV_FILE
# (~/e2sar_instance.env, shared $HOME). This script never reserves or frees the instance.
#
# Settings (environment variables, with the guide's defaults):
: "${ENV_FILE:=$HOME/e2sar_instance.env}" # EJFAT_URI: the LB instance URI. Don't free it
: "${SITE:=pm}"                          # sender site (pm = Perlmutter, lux = Lux): the RUNID prefix
                                         # and the default CONT and RUN, so each site's runs stay separate
: "${POOL:=sc26}"
: "${CONT:=e2sar-$SITE}"                 # DAOS container, shared by both receivers
: "${PORT:=20600}"                       # first receive port
: "${THREADS:=16}"                       # receive threads = ports (20600..20615)
: "${DEQ:=32}"                           # dequeue threads
: "${INFLIGHT:=256}"                     # DAOS puts in flight per dequeue thread
: "${BUF:=1073741824}"                   # receive socket buffer (-b)
: "${TIMEOUT:=10000}"                    # reassembly timeout, ms
: "${NIC:=enp1s0f1np1}"                  # data-plane NIC (drop counters; listen address if MY_IP is unset)
# MY_IP (from ~/.bashrc): the listen address on this node
: "${FIRST:=ebpf2201}"                   # the node that makes the RUNID
: "${RUN:=$HOME/e2sar_run/$SITE}"        # run directories (shared $HOME)
: "${RECV_ARGS:=}"                       # extra e2sar_daos_recv options
: "${E2SAR_DAOS_BUILD:=$(cd "$(dirname "$0")/.." && pwd)/build}"

source ~/.bashrc >/dev/null 2>&1 || true   # before set -u: .bashrc uses unset variables
set -u
: "${E2SAR_BIN:?E2SAR_BIN not set (see docs/ESNET_LB_TESTING.md step 1)}"
BIN=$E2SAR_DAOS_BUILD/bin
ME=$(hostname -s)

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
die() { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARNING: $*" >&2; }
lbadm() { "$E2SAR_BIN/lbadm" -v "$@"; }
nic_counters() { /usr/sbin/ethtool -S "$NIC" | grep -E ' (rx_packets_phy|rx_out_of_buffer|rx_discards_phy):'; }

load_uri() {
    [ -r "$ENV_FILE" ] || die "$ENV_FILE not found: it holds the LB instance URI (guide step 2)"
    source "$ENV_FILE"
    [ -n "${EJFAT_URI:-}" ] || die "EJFAT_URI is empty after sourcing $ENV_FILE"
    export EJFAT_URI
    echo "LB instance:  $(echo "$EJFAT_URI" | cut -d@ -f2)"
}

cmd_start() {
    load_uri
    [ -x "$BIN/e2sar_daos_recv" ] || die "$BIN/e2sar_daos_recv not built"
    # listen address: $MY_IP from ~/.bashrc, else the NIC's IPv4 address
    local ip=${MY_IP:-}
    [ -n "$ip" ] || ip=$(ip -4 -br addr show "$NIC" 2>/dev/null | awk '{print $3}' | cut -d/ -f1)
    [[ "$ip" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "listen address '$ip' isn't an IPv4 address (set MY_IP)"
    ip -4 -o addr show | grep -qw "$ip" || die "$ip (MY_IP) isn't an address of $ME: stale MY_IP?"
    echo "Node:         $ME ($NIC $ip)"

    # step 4: is the node idle?
    local busy
    busy=$(ps -eo user,pid,etime,args | grep -E 'e2sar_(ft|perf|daos_recv) ' | grep -v grep)
    [ -z "$busy" ] || die "e2sar processes already running on $ME:"$'\n'"$busy"
    local last=$((PORT + THREADS - 1))
    if ss -uanH | awk '{print $4}' | awk -F: -v a="$PORT" -v b="$last" '$NF>=a && $NF<=b {f=1} END {exit !f}'; then
        die "a UDP port in $PORT-$last is in use on $ME"
    fi
    local others
    others=$(lbadm --status | grep -o 'name=[^, ]*' | grep -v -E 'name=ebpf220[13]' || true)
    [ -z "$others" ] || die "other workers are registered on this LB instance; the LB would send them events:"$'\n'"$others"

    # step 5: the run directory, the same RUNID on both nodes
    mkdir -p "$RUN"
    local runid
    if [ "$ME" = "$FIRST" ]; then
        runid=${SITE}_$(date +%Y%m%d_%H%M%S)
        mkdir "$RUN/$runid" || die "can't create $RUN/$runid"
        ln -sfn "$runid" "$RUN/latest"
    else
        # wait for a run directory from $FIRST that this node hasn't joined yet
        echo "Waiting up to 5 min for $FIRST to start a new run in $RUN ..."
        local i
        for ((i = 0; i < 300; i++)); do
            runid=$(readlink "$RUN/latest" 2>/dev/null || true)
            if [ -n "$runid" ] && [ ! -e "$RUN/$runid/$ME.recv.log" ] &&
               [ -n "$(find "$RUN/$runid" -maxdepth 0 -mmin -10 2>/dev/null)" ]; then
                break
            fi
            runid=; sleep 1
        done
        [ -n "$runid" ] || die "no new run from $FIRST: start '$0 start' there first"
    fi
    local d=$RUN/$runid
    echo "Run:          $runid ($d)"
    echo "$runid" > "$d/$ME.runid"

    daos cont list "$POOL" 2>/dev/null | grep -qw "$CONT" &&
        echo "Container:    $POOL/$CONT exists (from the other node, or not cleaned up: '$0 clean')"

    nstat -n
    nic_counters > "$d/$ME.nic0"

    # Ctrl-C goes to the receiver, which deregisters and prints its statistics. tee and
    # this script ignore it, so the log keeps those statistics and step 7 runs afterwards.
    trap '' INT
    echo "Starting e2sar_daos_recv; Ctrl-C to stop it after the send"
    ( trap - INT; exec "$BIN/e2sar_daos_recv" -v --withcp --ip "$ip" \
        --port "$PORT" --threads "$THREADS" --pool "$POOL" --cont "$CONT" --prefix "$runid" \
        --timeout "$TIMEOUT" -b "$BUF" --deq "$DEQ" --inflight "$INFLIGHT" \
        --timing-csv "$d/$ME.csv" $RECV_ARGS ) 2>&1 | tee "$d/$ME.recv.log"

    # step 7: drops on this node
    echo
    echo "=== $ME: drop checks ==="
    nstat -z UdpRcvbufErrors | tee "$d/$ME.nstat"
    nic_counters > "$d/$ME.nic1"
    paste "$d/$ME.nic0" "$d/$ME.nic1" | awk '{print $1, "+" $4-$2}' | tee "$d/$ME.nicdiff"
    grep -E 'Lost in|Events (committed|failed)' "$d/$ME.recv.log"
    echo "Expect: UdpRcvbufErrors 0, rx_out_of_buffer and rx_discards_phy +0, losses 0, failed 0"
    echo "Logs in $d"
}

cmd_status() {
    load_uri
    lbadm --status | grep -E 'name=ebpf220[13]' | grep -o 'name=[^, ]*\|slotsassigned=[0-9]*' | paste - -
    echo "(send only when both ebpf2201 and ebpf2203 are listed with slotsassigned > 0)"
}

cmd_verify() {
    local n=${1:-} l=${2:-1048576}
    [[ "$n" =~ ^[0-9]+$ ]] || usage
    local runid d
    runid=$(readlink "$RUN/latest") || die "no $RUN/latest"
    d=$RUN/$runid
    echo "Verifying $runid: $n events of $l bytes in $POOL/$CONT"
    local fail=0
    "$BIN/e2sar_daos_read" --pool "$POOL" --cont "$CONT" --prefix "$runid" > "$d/events.list" || die "e2sar_daos_read failed"
    tail -1 "$d/events.list"

    # Loss is reported, not failed: events lost on the way never reach DAOS.
    # event rows only: 5 tab-separated columns (not the header or the "N committed events" line)
    awk -F'\t' 'NF==5 && $1 ~ /^[0-9]+$/' "$d/events.list" | cut -f1 | sort -n > "$d/events.nums"
    local got bad_num dup
    got=$(wc -l < "$d/events.nums")
    bad_num=$(awk -v n="$n" '$1 >= n' "$d/events.nums" | wc -l)
    dup=$(uniq -d "$d/events.nums" | wc -l)
    [ "$got" -gt 0 ] || { echo "FAIL no events in DAOS under '$runid/'"; exit 1; }
    [ "$bad_num" -eq 0 ] || { echo "FAIL $bad_num event numbers >= $n: wrong N, or two sends under one RUNID"; fail=1; }
    [ "$dup" -eq 0 ] || { echo "FAIL $dup event numbers listed twice"; fail=1; }
    comm -23 <(seq 0 $((n - 1)) | sort) <(sort -u "$d/events.nums") | sort -n > "$d/missing.txt"
    local lost
    lost=$(wc -l < "$d/missing.txt")
    local loss_line
    loss_line=$(awk -v g="$((n - lost))" -v n="$n" -v m="$lost" \
        'BEGIN {printf "%d of %d events (%d lost, %.2f%%)", g, n, m, 100 * m / n}')
    if [ "$lost" -eq 0 ]; then
        echo "OK   $loss_line"
    else
        echo "LOSS $loss_line; missing event numbers in $d/missing.txt, first ones: $(head -10 "$d/missing.txt" | tr '\n' ' ')"
    fi

    local bad
    bad=$(awk -F'\t' -v L="$l" 'NF==5 && $1 ~ /^[0-9]+$/ && $3!=L' "$d/events.list" | wc -l)
    [ "$bad" -eq 0 ] && echo "OK   every event $l bytes" || { echo "FAIL $bad events not $l bytes"; fail=1; }
    "$BIN/e2sar_daos_read" --pool "$POOL" --cont "$CONT" --prefix "$runid" --fetch > "$d/events.sha" || fail=1
    tail -1 "$d/events.sha"
    local digests
    digests=$(grep -E '^[0-9a-f]{64} ' "$d/events.sha" | cut -d' ' -f1 | sort -u | wc -l)
    [ "$digests" -eq 1 ] && echo "OK   one digest over all events" || { echo "FAIL $digests different digests"; fail=1; }
    # The receivers' commit counts: from the run catalog ("Receiver <host>: committed N events"
    # in the listing), else from the receivers' logs.
    local sum
    if grep -q '^Receiver .*: committed ' "$d/events.list"; then
        grep '^Receiver ' "$d/events.list"
        sum=$(sed -n 's/^Receiver .*: committed \([0-9]*\) events.*/\1/p' "$d/events.list" | awk '{s += $1} END {print s + 0}')
    else
        grep -H 'Events committed' "$d"/ebpf220[13].recv.log 2>/dev/null
        sum=$(grep -h 'Events committed' "$d"/ebpf220[13].recv.log 2>/dev/null | awk '{s += $NF} END {print s + 0}')
    fi
    # More commits than events in DAOS: an event was written twice (delivered to both receivers,
    # or two sends under one RUNID) and the later write overwrote the earlier one.
    if [ "$sum" -eq "$got" ]; then
        echo "OK   receivers committed $sum in total, as in DAOS"
    else
        echo "FAIL receivers committed $sum in total, DAOS has $got"; fail=1
    fi
    if [ $fail -eq 0 ]; then
        echo "PASS: every stored event is complete and correct; $loss_line"
    else
        echo "FAIL (details in $d); $loss_line"; exit 1
    fi
}

cmd_runs() {
    "$BIN/e2sar_daos_read" --pool "$POOL" --cont "$CONT" --prefixes | column -t -s $'\t'
}

cmd_clean() {
    local runid=${1:-}
    if [ -n "$runid" ]; then
        "$BIN/e2sar_daos_read" --pool "$POOL" --cont "$CONT" --prefix "$runid" --delete || exit 1
    else
        daos cont destroy "$POOL" "$CONT" >/dev/null 2>&1 && echo "destroyed $POOL/$CONT" || echo "$POOL/$CONT not present"
    fi
    load_uri
    lbadm --status
    echo "Expect no workers. If Perlmutter's IP is still a sender: $E2SAR_BIN/lbadm -v --removesenders -a <SEND_IP>"
    echo "Don't free the LB instance."
}

case ${1:-} in
    start)  cmd_start ;;
    status) cmd_status ;;
    verify) shift; cmd_verify "$@" ;;
    runs)   cmd_runs ;;
    clean)  shift; cmd_clean "$@" ;;
    *)      usage ;;
esac
