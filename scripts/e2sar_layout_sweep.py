#!/usr/bin/env python3
"""
e2sar_layout_sweep.py: tuning sweep of e2sar_daos_recv on the ebpf testbed, node to node,
driven from a machine that can SSH to both nodes (as e2sar_e2e_sweep.sh).

  e2sar_layout_sweep.py SCENARIO [SCENARIO ...] [--out results.jsonl] [--dry]

Scenarios (docs/STREAMING_WORKFLOW.md#tuning):
  a      e2sar_perf 20 KB events at 2.5 Gbps, one stream; --deq x --flush-us
  b      e2sar_ft: one 2 GB EVIO file at 5 Gbps (lossless), then several files unpaced (lossy)
  c1     e2sar_daos_recv --synthetic 1 MiB events, unpaced: the DAOS side alone; --deq x --inflight
  c2     8 e2sar_perf streams of 1 MiB events (one per receiver port), 20-80 Gbps; --deq
  c3     as c2 with 2 sender sockets per stream, 8 or 16 streams (receiver threads), 40-80 Gbps;
         --deq x --inflight
  s      e2sar_evio_send: the frames (~407 B) of one big EVIO file, 8 Segmenters, rising
         event rates; the receiver's event-rate limit. Not practical yet: e2sar_evio_send
         reads its file at ~12.5 MB/s, about an hour for the 44.7 GB file

Without the LB nothing spreads events over the receiver's ports, so each sender stream
goes to its own port (one reassembly thread each). Every run gets its own container,
destroyed after the run. One JSON line per run goes to --out.
"""

import argparse
import json
import re
import shlex
import subprocess
import sys
import time

SEND, RECV = "ebpf2201", "ebpf2203"
SEND_IP, RECV_IP, PORT = "129.57.178.85", "129.57.178.86", 20600
POOL = "sc26"
ENV = ("export E2SAR_ROOT=$HOME/opt/e2sar; "
       "export LD_LIBRARY_PATH=$E2SAR_ROOT/usr/local/lib64:$E2SAR_ROOT/usr/local/lib:$LD_LIBRARY_PATH; "
       "B=$E2SAR_ROOT/usr/local/bin; R=$HOME/daq/e2sar-daos/build/bin; SW=$HOME/e2sar_run/sweep")
NIC = "enp1s0f1np1"
FT_DIR = "/scratch/xmei/evio/ft"   # local copies on the sender node of the files e2sar_ft sends


def uri(port):
    return f"ejfat://useless@10.10.10.10:1234/lb/1?data={RECV_IP}:{port}"


def ssh(host, cmd, check=True, capture=True):
    r = subprocess.run(["ssh", "-x", host, "bash -c " + shlex.quote(ENV + "; " + cmd)],
                       capture_output=capture, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{host}: {cmd[:120]}...: rc {r.returncode}: {r.stderr[-500:]}")
    return r.stdout if capture else ""


def nic():
    out = ssh(RECV, f"/usr/sbin/ethtool -S {NIC} | grep -E ' (rx_discards_phy|rx_out_of_buffer):'")
    return {l.split(":")[0].strip(): int(l.split(":")[1]) for l in out.splitlines()}


def parse(log):
    def num(pat, cast=int, default=None):
        m = re.search(pat, log)
        return cast(m.group(1)) if m else default
    res = {
        "received": num(r"Events received:\s+(\d+)"),
        "committed": num(r"Events committed:\s+(\d+)"),
        "failed": num(r"Events failed:\s+(\d+)"),
        "batches": num(r"Batches committed:\s+(\d+)"),
        "lost_reassembly": num(r"Lost in reassembly:\s+(\d+)", default=0),
        "lost_enqueue": num(r"Lost in enqueue:\s+(\d+)", default=0),
        "ingest_gbps": num(r"Ingest throughput:\s+([\d.]+) Gbps", float),
        "commit_gbps": num(r"DAOS commit throughput:\s+([\d.]+) Gbps", float),
        "commit_window_s": num(r"Commit window:\s+([\d.]+) s", float),
    }
    for key, label in (("event", r"Event latency \(recv->commit\)"), ("write", "daos_array_write latency"),
                       ("index", "Index write latency")):
        m = re.search(label + r":.*?p50 ([\d.]+) \| p90 [\d.]+ \| p99 ([\d.]+) \| max ([\d.]+)", log)
        if m:
            res[f"{key}_p50_ms"], res[f"{key}_p99_ms"], res[f"{key}_max_ms"] = map(float, m.groups())
    m = re.search(r"CPU ([\d.]+) ([\d.]+) ([\d.]+)", log)
    if m:
        res["cpu_s"] = float(m.group(1)) + float(m.group(2))
    if res.get("committed") and res.get("batches"):
        res["events_per_batch"] = round(res["committed"] / res["batches"], 2)
    return res


def run(tag, recv_args, sender_cmd, expected, out, synthetic=False):
    cont = f"sw_{tag}_{int(time.time())}"
    d = f"$SW/{cont}"
    n0 = nic()
    recv = (f"mkdir -p {d}; TIMEFORMAT='CPU %U %S %R'; "
            f"{{ time $R/e2sar_daos_recv {recv_args} --pool {POOL} --cont-name {cont} --report 1 "
            f"> {d}/recv.log 2>&1 ; }} 2> {d}/time.txt")
    p = subprocess.Popen(["ssh", "-x", RECV, "bash -c " + shlex.quote(ENV + "; " + recv)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    if not synthetic:
        while "Receiving" not in ssh(RECV, f"cat {d}/recv.log 2>/dev/null", check=False):
            if p.poll() is not None or time.time() - t0 > 60:
                raise RuntimeError(f"{tag}: receiver didn't start: " + ssh(RECV, f"tail -3 {d}/recv.log", check=False))
            time.sleep(1)
        send_out = ssh(SEND, sender_cmd, check=False)
        last, idle = -1, 0
        while idle < 12:
            log = ssh(RECV, f"cat {d}/recv.log", check=False)
            c = [int(x) for x in re.findall(r"committed (\d+) evts", log)]
            n = c[-1] if c else 0
            if n >= expected:
                time.sleep(2)
                break
            idle = idle + 1 if n == last else 0
            last = n
            time.sleep(1)
        ssh(RECV, "pkill -INT -u $USER -x e2sar_daos_recv", check=False)
    else:
        send_out = ""
    p.wait()
    log = ssh(RECV, f"cat {d}/recv.log {d}/time.txt", check=False)
    n1 = nic()
    res = {"tag": tag, "container": cont, "expected": expected, **parse(log),
           "rx_discards_phy": n1.get("rx_discards_phy", 0) - n0.get("rx_discards_phy", 0),
           "rx_out_of_buffer": n1.get("rx_out_of_buffer", 0) - n0.get("rx_out_of_buffer", 0),
           "recv_args": recv_args}
    m = re.findall(r"Sent:?\s+([^\n]*)", send_out)
    if m:
        res["sender"] = m[-1][:120]
    if res.get("cpu_s") and res.get("commit_window_s"):
        res["cpu_cores"] = round(res["cpu_s"] / max(res["commit_window_s"], 1e-3), 2)
    res["lossless"] = res.get("committed") == expected
    ssh(RECV, f"daos container destroy {POOL} {cont} > /dev/null", check=False)
    with open(out, "a") as f:
        f.write(json.dumps(res) + "\n")
    print(f"{tag:28s} committed {res.get('committed')}/{expected} "
          f"commit {res.get('commit_gbps')} Gbps  evt/batch {res.get('events_per_batch')}  "
          f"lat p50/p99 {res.get('event_p50_ms')}/{res.get('event_p99_ms')} ms  "
          f"cpu {res.get('cpu_cores')} cores  loss r/e {res['lost_reassembly']}/{res['lost_enqueue']}  "
          f"nic {res['rx_discards_phy']}/{res['rx_out_of_buffer']}", flush=True)
    return res


def perf_senders(streams, length, gbps_each, n_each, sockets=8):
    cmds = [f"$B/e2sar_perf -s -u '{uri(PORT + i)}' --ip {SEND_IP} --mtu 9000 -l {length} -n {n_each} "
            f"--rate {gbps_each} --sockets {sockets} -b 67108864 --dataid {4321 + i} > /tmp/xm_perf_{i}.log 2>&1 &"
            for i in range(streams)]
    return " ".join(cmds) + " wait; grep -h 'Sent\\|sent' /tmp/xm_perf_*.log | tail -2; rm -f /tmp/xm_perf_*.log"


def recv_net(threads, deq, inflight=128, extra="", timeout=2000):
    return (f"-u '{uri(PORT)}' --ip {RECV_IP} --port {PORT} --threads {threads} --timeout {timeout} "
            f"-b 1073741824 --deq {deq} --inflight {inflight} {extra}")


def scenario_a(out):
    n = 300000  # 20 KB at 2.5 Gbps: ~15,600 events/s, ~19 s
    for deq in (1, 2, 4, 8):
        for flush in (1000, 5000, 20000):
            run(f"a_d{deq}_f{flush}", recv_net(1, deq, 32, f"--flush-us {flush}"),
                perf_senders(1, 20480, 2.5, n), n, out)


def scenario_b(out):
    one = f"{FT_DIR}/frames_thread0_file0019.evio"
    ft = "$B/e2sar_ft -s -u '{u}' --ip {ip} --mtu 9000 --rate {r} --sockets 4 -b 67108864 {files} 2>&1 | tail -3"
    # a 2 GB file takes 3.4 s at 5 Gbps: the reassembly timeout must be longer than that
    for deq, inflight, mw in ((4, 128, "64M"), (1, 16, "64M"), (4, 128, "16M"), (8, 128, "64M")):
        run(f"b_lossless_d{deq}_i{inflight}_mw{mw}", recv_net(1, deq, inflight, f"--max-write {mw}", 10000),
            ft.format(u=uri(PORT), ip=SEND_IP, r=5, files=one), 1, out)
    for rate in (20, -1):  # 4 files, paced at 20 Gbps and unpaced
        run(f"b_lossy_{'unpaced' if rate < 0 else str(rate) + 'g'}_d4", recv_net(1, 4, 128, "", 10000),
            ft.format(u=uri(PORT), ip=SEND_IP, r=rate, files=f"{FT_DIR}/*.evio"), 4, out)


def scenario_c1(out):
    for deq in (4, 8, 16, 32):
        for inflight in (32, 128):
            run(f"c1_d{deq}_i{inflight}", f"--synthetic 30000 --event-size 1M --deq {deq} --inflight {inflight}",
                "", 30000, out, synthetic=True)


def scenario_c2(out):
    streams, secs = 8, 15
    for total in (20, 40, 60, 80):
        each = total / streams
        n_each = int(each * 1e9 * secs / 8 / 1048576)
        for deq in (8, 16, 32):
            run(f"c2_{total}g_d{deq}", recv_net(streams, deq, 128),
                perf_senders(streams, 1048576, each, n_each), n_each * streams, out)


def scenario_c3(out):
    """As c2, with 2 sender sockets per stream (2 x the receiver threads in all), 8 or 16 streams."""
    secs = 15
    for streams in (8, 16):
        for total in (40, 60, 80):
            each = total / streams
            n_each = int(each * 1e9 * secs / 8 / 1048576)
            for deq in (8, 16):
                for inflight in (32, 128):
                    run(f"c3_t{streams}_{total}g_d{deq}_i{inflight}", recv_net(streams, deq, inflight),
                        perf_senders(streams, 1048576, each, n_each, sockets=2), n_each * streams, out)


BIG_EVIO = "/scratch/xmei/evio/frames_thread0_all.evio"   # 21 files joined with bin/evio_concat.py


def scenario_s(out):
    segs, secs = 8, 15
    for gbps in (0.5, 1.0, 2.0):
        n_each = int(gbps * 1e9 * secs / 8 / 407 / segs)
        for deq in (4, 8):
            send = (f"cd ~/daq/e2sar-daos && ./build/bin/e2sar_evio_send -u '{uri(PORT)}' --port-per-segmenter "
                    f"--mtu 9000 -P {segs} -s 1 -n {n_each} -F {BIG_EVIO} -r {gbps} 2>&1 | grep -E 'Sent:|goodput'")
            run(f"s_{gbps}g_d{deq}", recv_net(segs, deq, 128), send, n_each * segs, out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="+", choices=["a", "b", "c1", "c2", "c3", "s"])
    ap.add_argument("--out", default="layout_sweep.jsonl")
    a = ap.parse_args()
    for s in a.scenarios:
        {"a": scenario_a, "b": scenario_b, "c1": scenario_c1, "c2": scenario_c2, "c3": scenario_c3, "s": scenario_s}[s](a.out)


if __name__ == "__main__":
    sys.exit(main())
