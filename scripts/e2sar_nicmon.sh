#!/bin/bash
# e2sar_nicmon.sh: sample a receiver node's NIC, IP and UDP drop counters once a second during a run,
# so a burst of missing events can be placed: on the wire, in the NIC, in the kernel, or in a socket.
# Start it on every receiver node with the receivers, and stop it with them.
#
#   e2sar_nicmon.sh start DIR     snapshot DIR/nic_before_<host>.txt, then sample in the background
#                                 into DIR/nic_<host>.log (one line per second); refuses if already running
#   e2sar_nicmon.sh stop DIR      stop the sampler and snapshot DIR/nic_after_<host>.txt
#   e2sar_nicmon.sh report DIR    for this host's log in DIR: the change of each drop counter over the
#                                 run, every second in which one moved, and every second in which the NIC
#                                 received more than GAP packets that did not reach a UDP socket
#   e2sar_nicmon.sh sample        print one sample line and exit (to check the counters)
#
# A line is "<UTC epoch s> name=value ...", counters since boot. Names come from the counters' own
# headers, never from column positions:
#   NIC (ethtool -S $NIC):  rx_packets_phy (at the port), rx_packets (to the host), rx_discards_phy,
#                           rx_out_of_buffer, rx_steer_missed_packets (no matching receive rule),
#                           rx_vport_multicast_packets
#   IP  (/proc/net/snmp):   Ip.InReceives, Ip.InDiscards, Ip.InAddrErrors, Ip.InHdrErrors, Ip.InDelivers
#   UDP (/proc/net/snmp):   Udp.InDatagrams, Udp.NoPorts, Udp.InErrors, Udp.RcvbufErrors, Udp.InCsumErrors
#   TCP (/proc/net/snmp):   Tcp.InSegs, Tcp.InErrs (all TCP, any interface)
#   softnet:                softnet.dropped (sum over CPUs of /proc/net/softnet_stat column 2)
# Per second, rx_packets_phy - Udp.InDatagrams is everything on the port that isn't UDP to a socket.
# On the ebpf testbed the data port also carries NFS ($HOME, port 2049) and SSH, so this is a few
# hundred/s and can jump with file I/O; Tcp.InSegs shows how much of it is TCP (the counters are for
# all interfaces). Packets lost inside the host would show as rx_packets_phy - rx_packets (NIC) or
# Ip.InReceives - Ip.InDelivers, or as a UDP shortfall with no TCP to explain the gap.
#
# Settings: NIC (default enp1s0f1np1, the ebpf testbed data port), EVERY (1 s), GAP (3000).
# No root needed.

: "${NIC:=enp1s0f1np1}"
: "${EVERY:=1}"
: "${GAP:=3000}"
HOST=$(hostname -s)

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
die() { echo "ERROR: $*" >&2; exit 1; }

sample() {
    local nic
    nic=$(/usr/sbin/ethtool -S "$NIC" 2>/dev/null | awk '
        $1 ~ /^(rx_packets_phy|rx_packets|rx_discards_phy|rx_out_of_buffer|rx_steer_missed_packets|rx_vport_multicast_packets):$/ {
            sub(":", "", $1); printf " %s=%s", $1, $2 }')
    [ -n "$nic" ] || die "no counters from ethtool -S $NIC (set NIC)"
    printf '%s%s' "$(date +%s.%N | cut -c1-14)" "$nic"
    # /proc/net/snmp: a header line, then a value line, per protocol
    awk '$1 == "Ip:" || $1 == "Udp:" || $1 == "Tcp:" {
            p = substr($1, 1, length($1) - 1)
            if (!(p in hdr)) { for (i = 2; i <= NF; i++) h[p, i] = $i; hdr[p] = 1; next }
            for (i = 2; i <= NF; i++) {
                k = p "." h[p, i]
                if (k ~ /^Ip\.(InReceives|InDiscards|InAddrErrors|InHdrErrors|InDelivers)$/ ||
                    k ~ /^Udp\.(InDatagrams|NoPorts|InErrors|RcvbufErrors|InCsumErrors)$/ ||
                    k ~ /^Tcp\.(InSegs|InErrs)$/) printf " %s=%s", k, $i
            } }' /proc/net/snmp
    awk '{ d += strtonum("0x" $2) } END { printf " softnet.dropped=%d\n", d }' /proc/net/softnet_stat
}

pidfile() { echo "$1/nicmon_$HOST.pid"; }

case "${1:-}" in
    sample)
        sample ;;
    start)
        D=${2:-}; [ -n "$D" ] || usage
        mkdir -p "$D"
        P=$(pidfile "$D")
        if [ -s "$P" ] && kill -0 "$(cat "$P")" 2>/dev/null; then die "already sampling (pid $(cat "$P"))"; fi
        sample > "$D/nic_before_$HOST.txt" || exit 1
        # setsid: keeps sampling if the starting shell or SSH session ends
        setsid bash -c "while :; do bash \"$0\" sample; sleep $EVERY; done" >> "$D/nic_$HOST.log" 2>&1 < /dev/null &
        echo $! > "$P"
        echo "[nicmon $HOST] sampling $NIC every $EVERY s into $D/nic_$HOST.log (pid $!)" ;;
    stop)
        D=${2:-}; [ -n "$D" ] || usage
        P=$(pidfile "$D")
        if [ -s "$P" ]; then
            # the loop runs in its own session: stop the whole group
            kill -TERM -- -"$(cat "$P")" 2>/dev/null || kill -TERM "$(cat "$P")" 2>/dev/null
            rm -f "$P"
        else
            echo "[nicmon $HOST] not running in $D" >&2
        fi
        sample > "$D/nic_after_$HOST.txt"
        echo "[nicmon $HOST] stopped; $(grep -c '^[0-9]' "$D/nic_$HOST.log" 2>/dev/null || echo 0) samples in $D/nic_$HOST.log" ;;
    report)
        D=${2:-}; [ -n "$D" ] || usage
        L=$D/nic_$HOST.log; [ -s "$L" ] || die "no $L"
        awk -v gap="$GAP" '
            /^[0-9]/ {
                t = int($1); delete v
                for (i = 2; i <= NF; i++) { split($i, a, "="); v[a[1]] = a[2] }
                if (n++) {
                    moved = ""
                    for (k in v) if ((k in p) && k ~ /discards|out_of_buffer|steer_missed|Errors|InDiscards|dropped/ && v[k] != p[k]) {
                        tot[k] += v[k] - p[k]; moved = moved " " k "+" (v[k] - p[k])
                    }
                    if (moved != "") print strftime("%T", t, 1), "drops:" moved
                    if (("rx_packets_phy" in p) && ("Udp.InDatagrams" in p)) {
                        g = (v["rx_packets_phy"] - p["rx_packets_phy"]) - (v["Udp.InDatagrams"] - p["Udp.InDatagrams"])
                        if (g > gap) print strftime("%T", t, 1), "not delivered to UDP:", g, "packets (NIC",
                            v["rx_packets_phy"] - p["rx_packets_phy"], "UDP", v["Udp.InDatagrams"] - p["Udp.InDatagrams"], "TCP", v["Tcp.InSegs"] - p["Tcp.InSegs"] ")"
                    }
                } else first = t
                for (k in v) p[k] = v[k]; last = t
            }
            END {
                printf "%d samples, %s to %s UTC\n", n, strftime("%T", first, 1), strftime("%T", last, 1)
                s = ""; for (k in tot) s = s " " k "+" tot[k]
                print "drop counters over the run:" (s == "" ? " none moved" : s)
            }' "$L" ;;
    *)
        usage ;;
esac
