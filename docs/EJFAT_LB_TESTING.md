# Testing through the ESnet EJFAT load balancer

This is the procedure for sending with `e2sar_ft` from ebpf2201, through the ESnet EJFAT load balancer (LB), to `e2sar_daos_recv` on ebpf2203, which writes into DAOS.
It reuses the environment, build and verification steps of [EBPF_TESTBED_NOTES.md](EBPF_TESTBED_NOTES.md). This page covers only what changes when a real LB is in the path.
Last run on 2026-10-19 against `feature/daos-kv-recv-apps`, LB instance 638 on `ejfat-lb.es.net`.

```
ebpf2201 (129.57.178.85)        ESnet LB 638                        ebpf2203 (129.57.178.86)
e2sar_ft -s --withcp  ──UDP──▶ data 192.188.29.16:16384-32767 ──UDP──▶ e2sar_daos_recv --withcp :20600 ──▶ DAOS sc26/e2sar-lb
          └── sync ──────────▶ sync 192.188.29.6:19016
          └── gRPC (add/remove sender) ─▶ ejfat-lb.es.net:18008 ◀── gRPC (register, state, deregister)
```

## 1. What is different from back-to-back

| | Back-to-back ([notes](EBPF_TESTBED_NOTES.md)) | Through the LB |
|---|---|---|
| `EJFAT_URI` | Hand-written, `data=<receiver ip>:$PORT` | The reserved instance URI from the LB, with the instance token |
| Both apps | No `--withcp` | `--withcp` on sender and receiver |
| Receiver | Listens; nobody needs to know about it | Registers as a worker (hostname, `--ip`, `--port`) and gets slots; deregisters on Ctrl-C |
| Sender | Sends straight to the receiver | Adds its `--ip` to the LB sender allowlist, sends to the LB data address, removes itself on exit |
| LB header | Kept; the receiver strips it | Stripped by the LB (`useCP` on) |
| Path | One switch hop | WAN, ~71 ms RTT from the ebpf nodes; path MTU 9000 works |

## 2. Tokens and URIs

There are two kinds of URI. Keep both out of git.

- **Admin URI**, `ejfats://<admin token>@ejfat-lb.es.net:18008/lb/0`. Only for `lbadm --reserve`, `--free` and `--overview`. The apps don't need it.
- **Instance URI**, which `lbadm --reserve` returns. It carries the instance token, the LB id, the sync addresses and the data address range:

```bash
export EJFAT_URI='ejfats://<instance token>@ejfat-lb.es.net:18008/lb/<id>?sync=192.188.29.6:19016&sync=[2001:400:a300::6]:19016&data=192.188.29.16:16384-32767&data=[2001:400:a300::16]:16384-32767'
```

Both apps and `lbadm --status` use the instance URI. Unlike the back-to-back case, the `data=` port range is right as the LB gives it. The LB, not the URI, picks the worker port.

If no instance is reserved yet, reserve one with the admin URI (not run in this test; LB 638 was already reserved). `-a` lists sender IPs, and the reservation can be freed with `--free`:

```bash
EJFAT_URI='<admin URI>' $E2SAR_BIN/lbadm --reserve -l <name> -a 129.57.178.85 -d 24:00:00 -e    # -e prints export EJFAT_URI=...
```

## 3. Environment (each shell, both nodes)

Start from step 1 of the [testbed notes](EBPF_TESTBED_NOTES.md#1-environment-each-shell-both-nodes) and override the following. The separate port, container and directories keep an LB run from colliding with a back-to-back run on the same nodes.

```bash
export EJFAT_URI='<instance URI>'
export PORT=20600 CONT=e2sar-lb
export SRC=$SCR/e2sar_src_lb OUT=$SCR/e2sar_out_lb
```

Check reachability once from each node:

```bash
ip route get 192.188.29.16                        # expect: dev enp1s0f1np1 src 129.57.178.8x
ping -c2 -M do -s 8972 192.188.29.16              # MTU 9000 path; ~71 ms
$E2SAR_BIN/lbadm --status                         # LB details, registered senders, registered workers
```

Stage the source files on ebpf2201 as in [step 3 of the notes](EBPF_TESTBED_NOTES.md#3-source-files-before-each-run-on-the-sending-node), with `$SRC` as set above. Save the digests to `$RUN/src_lb.sha256`.

## 4. Run

Use a new `PREFIX` for every sender run. `e2sar_ft` numbers events from 0 each time, so a second send under the same prefix overwrites the first.

**ebpf2203, receiver** (start it first):
```bash
export PREFIX=lb_$(date +%s)
cd ~/daq/e2sar-daos
./build/bin/e2sar_daos_recv --withcp --ip 129.57.178.86 --port $PORT \
    --pool sc26 --cont $CONT --prefix $PREFIX \
    --timeout 10000 -b 1073741824 --verbose
```
It should print `Control plane: ON` and `Listening on: 129.57.178.86 ports 20600:20600`.
Wait about 10 s, then check that the LB has given it slots:
```bash
$E2SAR_BIN/lbadm --status | grep slotsassigned    # name=ebpf2203.jlab.org, ..., slotsassigned=512, ..., UDP Port=20600
```

**ebpf2201, sender:**
```bash
$E2SAR_BIN/e2sar_ft -s --withcp --ip 129.57.178.85 --mtu 9000 --rate 5 -p $SRC/*
```
Expected: `Adding senders to LB: 129.57.178.85 ... done`, one `Queueing file` per file, `0 errors`, and `Removing senders: 129.57.178.85` at the end.
The `--ip` you pass is the address that goes on the allowlist, so it has to be the address the packets leave from. You don't need `lbadm --addsenders` when the sender runs with `--withcp`.

Press Ctrl-C on the receiver after the last `committed` line. It deregisters, so `lbadm --status` shows no workers again.
Then verify on ebpf2203 with [step 7 of the notes](EBPF_TESTBED_NOTES.md#7-verify-and-restore-on-the-receiver), using `--cont $CONT` and `$RUN/src_lb.sha256`.

## 5. Clean up

```bash
# ebpf2203
rm -rf $OUT
daos cont destroy sc26 $CONT
# ebpf2201
rm -rf $SRC
# either node: no workers left, and only the senders that were there before
$E2SAR_BIN/lbadm --status
```
Free the LB with `lbadm --free` only if you reserved it yourself and nobody else is using it.

## Results from 2026-10-19 (LB 638, MTU 9000, `--rate 5`, `-b 1 GiB`)

Each full run sent `frames_thread0_file0020.evio` (1.78 GB) and the 3.5 MB random file. Every run passed: digests matched, the EVIO file read as `EVIO v6 big-endian`, and there were 0 lost events and 0 `UdpRcvbufErrors`.

| Run | Files | Sender goodput | Receive window | Ingest | DAOS commit |
|---|---|---|---|---|---|
| 1 | random only | (one 3.5 MB event) | | | committed 1 of 1 |
| 2 | both | 7.12 Gbps | 1.53 s | 9.30 Gbps | 8.53 Gbps |
| 3 | both | 7.12 Gbps | 1.47 s | 9.70 Gbps | 9.01 Gbps |

The throughput is the same as the back-to-back ebpf2201 → ebpf2203 runs (7.3–7.4 Gbps sent, 8.8–8.9 Gbps ingest).
So the LB and the 71 ms WAN path don't lower throughput here. The single-threaded segmenter limit described in [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md) still sets the ceiling.

## Troubleshooting

Only the first row was seen in these runs. The others are what to check first, based on how the LB and E2SAR work.

| Symptom | Cause and fix |
|---|---|
| Receiver: `Unable to open receive sockets: Address already in use` | Another receiver holds `$PORT`. Check with `ss -uanp \| grep :$PORT`, and pick another port rather than killing someone else's run |
| Receiver runs, but `lbadm --status` lists no worker | It was started without `--withcp`, or it exited. Read its output |
| Worker listed with `slotsassigned=0` | The LB hasn't recomputed the schedule yet. Wait a few seconds before sending |
| Sender `0 errors`, but the receiver gets nothing | The sender's `--ip` isn't the address the packets leave from, so the LB drops them as from an unknown sender. Check `ip route get 192.188.29.16`. Also check that inbound UDP to the receiver port isn't firewalled |
| Receiver gets events that you didn't send | Another sender on the allowlist is sending on this LB instance. The `--prefix` keeps them apart in DAOS, but use your own reservation for clean numbers |
| gRPC or certificate errors | The URI must be `ejfats://` with the hostname. Use `-v` (`--novalidate`) only if the CA isn't installed |
