---
type: Result
title: "The network path LUX → ESnet LB → JLab: MTU, RTT and routes (2026-11-23)"
description: "Path checks from a LUX compute node (lux281, OLCF) and from ebpf2201 (JLab) to the LB data-plane addresses 192.188.29.11 and .12. MTU 9000 passes unfragmented on both halves. RTT LUX → LB 60.5 ms, ebpf2201 → LB 70.8 ms (.12) and 75.6 ms (.11), so an event takes ~66–68 ms one way from LUX to JLab. LUX reaches the LB on frontend.4 (source 198.124.49.31) but JLab on bond0 (private 10.139.x, MTU 1500): there is no direct jumbo path between LUX and JLab, only through the LB. With the BDP and Mathis limits for TCP at 40–100 Gbps on these RTTs."
tags: [lux, olcf, lb, ejfat, ebpf-testbed, network, mtu, latency]
sources:
  - { id: lcf, resource: "../sites/lcf-slurm.md", title: "LCFs using Slurm (Perlmutter, LUX)" }
  - { id: summary, resource: "e2e-summary.md", title: "End-to-end results: summary" }
---

# The network path LUX → ESnet LB → JLab: MTU, RTT and routes (2026-11-23)

Before sending from LUX through the ESnet LB to the receivers at JLab: does MTU 9000 pass on each half of the path, what is the round trip, and which routes do the packets take. No data was sent.

```
lux281 (OLCF)                         ESnet LB data plane                 ebpf2201 (JLab)
frontend.4 198.124.49.31, MTU 9000    192.188.29.11 / .12                 enp1s0f1np1 129.57.178.85, MTU 9000
   ── gw 198.124.48.1 ── (hidden) ──▶   ──▶ … 198.129.78.161 … 134.55.x … (hidden) ── gw 129.57.178.65 ──▶
         RTT 60.5 ms                       RTT 70.8 ms (.12) / 75.6 ms (.11)
```

## Setup

| | |
|---|---|
| LUX node | lux281, one node of the `ejfat_demo` reservation (see [LUX in the LCF guide](../sites/lcf-slurm.md#lux-olcf)), from `srun -A csc741 --reservation=ejfat_demo -p batch -N 1 -t 01:00:00 --gpus=1 --pty bash` |
| JLab node | ebpf2201, data-plane NIC `enp1s0f1np1` (129.57.178.85) |
| LB | the data-plane addresses 192.188.29.11 and 192.188.29.12 |
| Tools | `ip route get`, `ping -M do -s 8972` (9000-byte packets, fragmenting forbidden) and `-s 1472` (1500), `tracepath -n` |

Each half was measured from its end toward the LB. The LB only forwards E2SAR data from registered senders, so no probe can cross it: the LB → JLab half was measured in the reverse direction, JLab → LB. The two directions may take different routes; the RTT is the same.

## Results

| Half | From | Interface, source | 9000-byte ping, no fragmenting | RTT min / avg / max | Path MTU (`tracepath`) |
|---|---|---|---|---|---|
| LUX → LB .11 | lux281 | `frontend.4`, 198.124.49.31 | 3/3, 0% loss (twice) | 60.29 / 60.44 / 60.52 ms | 9000 at lux281; no hop answers after the gateway |
| LUX → LB .12 | lux281 | `frontend.4`, 198.124.49.31 | 3/3, 0% loss (twice) | 60.51 / 60.52 / 60.54 ms | as above |
| JLab → LB .11 | ebpf2201 | `enp1s0f1np1`, 129.57.178.85 | 5/5, 0% loss | 75.44 / 75.58 / 75.66 ms | 9000 |
| JLab → LB .12 | ebpf2201 | `enp1s0f1np1`, 129.57.178.85 | 5/5, 0% loss | 70.75 / 70.76 / 70.79 ms | 9000 |

- **MTU 9000 passes on both halves.** Every 9000-byte ping with fragmenting forbidden came back full size, so no device on either half, answering or not, is below 9000. Send from LUX with MTU 9000.
- **The 1500-byte pings took the same time as the 9000-byte ones** (60.5 ms), and the jitter (mdev) was 0.006–0.1 ms: no queueing on the path at the time.
- The LB answered with TTL 34 (.11) and 33 (.12), so .12 is likely one hop further on the way back or answers from another device. It makes no difference to the data.

### From LUX to JLab through the LB

| Through | LUX → LB RTT | LB → JLab RTT | Sum (a round trip through the LB) | One way, LUX → JLab |
|---|---|---|---|---|
| .12 | 60.5 ms | 70.8 ms | 131.3 ms | **~66 ms** |
| .11 | 60.5 ms | 75.6 ms | 136.1 ms | **~68 ms** |

One way is half the sum, an estimate from two halves measured separately. E2SAR sends over UDP and never waits for an answer from the receiver, so the one-way time is what an event takes from LUX to JLab; the round trip through the LB never happens on the data path.

## Packet sizes in earlier LUX runs

The data bytes committed divided by the packets the receiver NICs took in during each run:

| Runs | Sender | Data per packet | MTU used |
|---|---|---|---|
| `lux_20261028_*`, `lux_20261029_*`: `e2sar_perf`, 1 MiB events, 15–26 Gbps (not written up) | one LUX node | 8,610–8,838 B | 9000 |
| [LB 682](2026-11-09-lb682-perlmutter-losses.md) EVIO frames | LUX and Perlmutter frame builders | 1,498-byte frames in the captures | 1500 |
| LB 686 runs 1794441647, 1794444385 (~26 KB events) | 198.124.49.31 (LUX) and 129.57.177.19 | 1,397–1,398 B | 1500 |
| LB 686 runs [1794459212](2026-11-12-045334UTC-lb686-two-workers-10tb.md), 1794515182, [1794515844](2026-11-12-203725UTC-lb686-40gbps-10tb.md) (171–387 KB events) | 198.124.49.31, .32 and 129.57.177.19 | 1,430–1,433 B | 1500 |

At MTU 1500 an E2SAR packet carries at most 1,436 B of event data, at 9000 8,936 B. The LB replaces the source address, so on LB 686 the share of each sender can't be told apart: none of them sent above 1500. Since the path carries 9000 on both halves, the frame builders on LB 682 and LB 686 could send ~6× fewer packets for the same rate.

## The routes

**LUX → LB.** `ip route get 192.188.29.11` (and .12) on lux281:

```
192.188.29.11 via 198.124.48.1 dev frontend.4 src 198.124.49.31
```

`tracepath` gets an answer from the gateway 198.124.48.1 (0.25–0.4 ms) and from no hop after it, up to 20: the probes are filtered past the gateway. The route on the LUX side is hidden; only OLCF can give it.

**JLab → LB.** `tracepath -n` from ebpf2201, the same for .11 and .12:

| Hop | Address | RTT | Likely |
|---|---|---|---|
| 1 | 129.57.178.65 | 0.2 ms | ebpf2201's data-plane gateway |
| 2 | no answer | | JLab border |
| 3–9 | 134.55.57.234, .57.197, .57.195, .56.14, .56.96, .56.225, .56.230 | ~76 ms | ESnet (address range, not checked) |
| 10 | no answer | | |
| 11 | 198.129.78.161 | ~76 ms | the last router answering before the LB |
| 12–30 | no answer | | the LB answers pings but not `tracepath` |

The RTT jumps from 0.2 ms to 76 ms between hop 1 and hop 3, and every ESnet hop after that is at the same 76 ms: the long-distance part is one hidden stretch, and the answering hops are near the LB.

A trace shows only routers (layer 3): each one lowers the TTL and answers when it reaches zero. Switches between them don't show. Only the network operators (OLCF, JLab, ESnet) can give the devices in between.

## No direct path between LUX and JLab

LUX sends to JLab on a different interface than to the LB:

```
# on lux281
129.57.178.85 via 10.139.128.1 dev bond0 src 10.139.130.9
```

| From lux281 to | Interface | Source | MTU |
|---|---|---|---|
| The LB, 192.188.29.11 / .12 | `frontend.4` | 198.124.49.31 (public) | 9000 |
| ebpf2201, 129.57.178.85 | `bond0` | 10.139.130.9 (private) | 1500 |

- `ping -M do -s 8972 129.57.178.85` on lux281 fails at once: `ping: local error: message too long, mtu=1500`. The 9000-byte packet never leaves the node.
- From ebpf2201, pings to lux281 (198.124.49.31), its gateway 198.124.48.1, `lux.olcf.ornl.gov` and `home.ccs.ornl.gov` all get no answer (100% loss, small and 9000-byte). `tracepath` to `lux.olcf.ornl.gov` goes through JLab's internal 172.31.x routers (not the data-plane route to the LB) and gets no answer after hop 5, 172.31.0.202.

So the only path between LUX and JLab with MTU 9000 is through the LB. LUX → LB → JLab doesn't need a direct path, but anything sent straight between the two sites (iperf3, node-to-node E2SAR) would go over `bond0` with MTU 1500, if at all. A direct jumbo path would need OLCF to route JLab's 129.57.0.0/16 over `frontend.4` (`ip route show dev frontend.4` lists what it carries).

## What the RTT means for TCP

The data goes over UDP, so none of this limits E2SAR: it matters for TCP over the same path, e.g. an `iperf3` baseline. One TCP stream needs a window of at least the bandwidth-delay product, BDP = rate × RTT, and its rate is capped by loss (Mathis): rate ≲ (MSS / RTT) × 1.22 / √p, with p the packet loss rate. With MSS = 8972 bytes (the real TCP MSS at MTU 9000 is 8948, 0.3% less):

| | LUX → LB, 60.5 ms | JLab → LB, 75.6 ms | LUX → LB → JLab, 136.1 ms |
|---|---|---|---|
| BDP at 40 Gbps | 302 MB (33.7k packets) | 378 MB | 680 MB |
| BDP at 100 Gbps | 756 MB (84.3k packets) | 945 MB | 1.70 GB |
| Highest loss for 40 Gbps | 1.3×10⁻⁹ | 8.4×10⁻¹⁰ | 2.6×10⁻¹⁰ |
| Highest loss for 100 Gbps | 2.1×10⁻¹⁰ | 1.3×10⁻¹⁰ | 4.1×10⁻¹¹ |
| Rate at 10⁻⁶ loss | 1.45 Gbps | 1.16 Gbps | 0.64 Gbps |
| Rate at 10⁻⁸ loss | 14.5 Gbps | 11.6 Gbps | 6.4 Gbps |

The last column would apply only to a TCP connection through the LB both ways, which the LB doesn't carry. N parallel streams divide the window by N and raise the tolerable loss by N², which is why long-path TCP tests use `iperf3 -P 16` or more. For E2SAR what counts is the packet rate at the receiver (0.56M packets/s at 40 Gbps, 1.39M at 100 Gbps at MTU 9000) and that the path loses nothing.
