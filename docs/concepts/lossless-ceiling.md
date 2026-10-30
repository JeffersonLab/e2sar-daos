---
type: Concept
title: "The lossless ceiling and the port discards"
description: "Why runs are lossless only up to ~25 Gbps node-to-node and 25–35 Gbps through the LB: NIC port discards on ebpf2203, the possible causes, and the runs that would tell them apart."
tags: [loss, nic, rx_discards_phy, ebpf-testbed]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/E2E_BENCHMARK.md", title: "E2E_BENCHMARK.md at 0d85d00" }
---

# The lossless ceiling and the port discards

## Why the lossless ceiling is ~25 Gbps node-to-node and 25–35 through the LB

- **Node-to-node, most of the loss is at the start of a run** ([section 7](../results/2026-10-20-e2e-n2n-sweep.md#7-node-to-node-rate-sweep-2050-gbps-2026-10-20)): after the first 0.5 s, 39 of 42 runs at 20–50 Gbps lost nothing. The ceiling for a whole 100,000-event run is set by those first moments.

- **NIC port discards on ebpf2203** (`rx_discards_phy`), as in [2026-10-19-bottleneck-e2sar-ft](../results/2026-10-19-bottleneck-e2sar-ft.md). The switch doesn't pass ebpf2203's pause frames back to the sender, so short overloads drop frames, and one lost frame loses its whole event.
- **Bursts, not the average rate.** Each event leaves the sender as a line-rate burst. Node-to-node, those bursts reach ebpf2203 as they are. Through the LB (ESnet WAN and the LB's data plane) they arrive smoothed, so the same average rate drops fewer frames: 20 Gbps passed 3 of 3 through the LB but 1 of 3 node-to-node.
- **Possibly made worse by DAOS on the same adapter.** E2SAR arrives on `enp1s0f1np1` (Ethernet, 100 Gb/s) and DAOS writes go out on `ibp1s0f0` (InfiniBand HDR, 200 Gb/s). They're the two ports of **one ConnectX-6** (PCI `01:00.0` and `01:00.1`, PCIe Gen4 x16, NUMA node 0), so they share its host interface and on-card buffers.
  The one paced 40 Gbps run that had 0 discards wrote to DAOS at only 24 Gbps (4 dequeue threads). At 25 Gbps and more written to DAOS, there were nearly always some. This is a correlation, not a proof.
  `e2sar_perf` with no DAOS at all also gets discards from 30 Gbps node-to-node ([section 10](../results/2026-10-26-e2sar-perf-n2n-vs-lb.md#10-e2sar_perf-without-daos-node-to-node-vs-lb-1090-gbps-2026-10-26-and-2026-10-28)), so DAOS isn't needed for them.
- **Not the cause:** DAOS (writes take 0.2–0.5 ms p50 at every rate, no failed puts), socket buffers (1 GiB, no `RcvbufErrors`), the RX ring (8192, no `rx_out_of_buffer`).

To go higher without loss: flow control or a deeper buffer at ebpf2203's switch port, pacing inside each event (the staged E2SAR's `--smooth` is too slow, below 1 Gbps), DAOS on a separate adapter (worth testing), or several receiver nodes behind the LB.

Which of these explanations holds, and how much each contributes, is open. The next section lays out how to tell them apart.

## Diagnosing the port discards

What is known about the discards, what isn't, and the runs that would separate the possible causes.

### Where a frame can be lost

| Where | Counter (on ebpf2203 unless stated) | In these runs |
|---|---|---|
| Switch, egress port toward ebpf2203 | The switch's interface discard and pause counters. Ask the network team | Not read |
| **NIC port buffer, before the host** | `rx_discards_phy` | **In every failed run** |
| NIC receive ring, no host buffer posted | `rx_out_of_buffer` | 0 |
| Kernel UDP socket | `UdpRcvbufErrors` (`nstat`) | 0 |
| E2SAR reassembly | `Lost in reassembly` | Only as a result of the above |
| DAOS | Failed puts, `Lost in enqueue` | 0 |

`rx_discards_phy` counts frames the port dropped because its on-card buffer was full. The NIC couldn't move them into host memory as fast as they arrived.
The receive ring always had buffers (`rx_out_of_buffer` 0), so software wasn't holding things up. The hold-up is between the port buffer and host memory: the NIC's receive pipeline and its DMA writes over PCIe.

### What the evidence says so far

| Finding | Source | What it shows |
|---|---|---|
| TCP into ebpf2203 reaches 98 Gbps (iperf3, 6–12 streams, MTU 9000) | iperf3 sweep, 2026-10-15, DAOS idle | The link, the port and the PCIe path sustain about line rate. The discards are short overloads, not a bandwidth ceiling |
| UDP into ebpf2203 (iperf3, 4 streams) received ~20 Gbps for every offered rate from 25 to 80 Gbps, with 0 loss | same | The iperf3 sender couldn't go past ~20 Gbps, so this is no baseline above 20 Gbps. The nightly job uses the same kind of sender and will hit the same limit |
| The same 20 Gbps traffic: lossless in 3 of 3 runs through the LB, 1 of 3 node-to-node | [section 5](../results/2026-10-19-e2e-bench.md#5-confirmation-runs-with-the-defaults) | Bursts matter, not the average rate |
| ebpf2203 has sent pause frames (820), ebpf2201 has received none | [2026-10-19-bottleneck-e2sar-ft](../results/2026-10-19-bottleneck-e2sar-ft.md#8-parallel-senders-with-rx-ring-8192-and-pinned-receive-threads) | Node-to-node traffic also crosses a switch. ebpf2203 asks for backpressure but it doesn't reach the sender. Pause is per hop, so enabling it on the hosts alone can't fix this. Not known whether the switch honors the pause and buffers, or ignores it |
| 40 Gbps on the wire. With 4 dequeue threads (24 Gbps into DAOS): 0 discards. With 16 (40 Gbps into DAOS): 92 | [section 3](../results/2026-10-19-e2e-bench.md#3-dequeue-threads-the-hidden-bottleneck) | Points to DAOS writes on the other port of the same ConnectX-6. One run each |
| `e2sar_perf` without DAOS, 16 × 1 MiB streams: every run that lost events had `rx_discards_phy` > 0, and nothing else dropped. Node-to-node without recvmmsg, from 30 to 90 Gbps nearly all loss is among the first ~100 events | [section 10](../results/2026-10-26-e2sar-perf-n2n-vs-lb.md#10-e2sar_perf-without-daos-node-to-node-vs-lb-1090-gbps-2026-10-26-and-2026-10-28), [2026-10-19-bottleneck-e2sar-ft table 9](../results/2026-10-19-bottleneck-e2sar-ft.md#9-e2sar-alone-without-daos-e2sar_perf-node-to-node-vs-lb) | The discards don't need DAOS writes on the same adapter (cause 2 isn't necessary, though it may add to them). Start-up loss happens without DAOS too |
| `e2sar_perf` through LB 86 at 20–30 Gbps: 40–78 events lost per run, all through the run, with 35–640 discards. Node-to-node 20 Gbps: lossless | same | Goes against "the LB smooths the bursts" for this setup (32 ports, a different LB instance). Open |
| 1.5 MB events, node-to-node: 4,215 and 1,187 discards at 20 Gbps, but 267 and 121 at 40 Gbps | [section 6](../results/2026-10-19-e2e-bench.md#6-daos-layout-chunks-or-one-value-per-event) | Unexplained. Fewer discards at the higher rate goes against a simple rate effect. Possibly the Segmenters' bursts line up more often at 20 Gbps. Needs repeats |

### Possible causes

1. **Bursts that overflow the port buffer.** Several Segmenters' line-rate event bursts arrive at the same time, faster than the NIC drains them for a moment. This doesn't need DAOS at all. Predicts: discards grow with burst size (frames per event) and with the number of Segmenters at the same total rate, and fall when the bursts are smoothed (the LB).
2. **DAOS traffic on the same adapter.** E2SAR arrives on Ethernet port 1 and DAOS writes leave on IB port 0 of one ConnectX-6 (PCI `01:00.0`/`01:00.1`). The two directions use opposite directions of the PCIe link, so raw PCIe bandwidth is not the likely shared resource. More likely: the NIC's internal processing, or the PCIe reads for outgoing DAOS data briefly stalling the receive DMA. Predicts: discards grow with the DAOS write rate at the same wire rate, also with plain TCP or UDP traffic in place of E2SAR.
3. **Host DMA stalls not caused by DAOS.** IOMMU translation misses, memory or PCIe settings. Predicts: the NIC's PCIe stall counters rise together with discards even when DAOS is idle.
4. **The switch.** If the switch port toward ebpf2203 ignores pause frames, or has a small egress buffer, every burst overload ends at ebpf2203's port. This doesn't cause the overload, but it decides whether an overload becomes loss.

These can act together. Cause 1 is certainly part of it (the LB result). The question is whether 2 or 3 add to it.

### Counters to record in every diagnosis run

On ebpf2203, before and after each run, while the nodes are otherwise idle:

```bash
NIC=enp1s0f1np1
IBDEV=$(ibdev2netdev | awk '$5 == "ibp1s0f0" {print $1}')     # the IB device DAOS uses
snap() {
  ethtool -S $NIC | grep -E ' (rx_discards_phy|rx_out_of_buffer|rx_buffer_passed_thres_phy|tx_pause_ctrl_phy|outbound_pci_stalled_(rd|wr)(_events)?):'
  echo "ib_port_xmit_data_words: $(cat /sys/class/infiniband/$IBDEV/ports/1/counters/port_xmit_data)"
}
snap > before; nstat -n
#   ... run ...
snap > after;  nstat -z UdpRcvbufErrors
paste before after | awk '{print $1, $4 - $2}'
```

On ebpf2201: `ethtool -S enp1s0f1np1 | grep rx_pause_ctrl_phy`, before and after.

| Counter | Meaning |
|---|---|
| `rx_discards_phy` | Frames dropped at the port: the loss itself |
| `rx_buffer_passed_thres_phy` | Times the port buffer passed its fill threshold. Rising without discards means near misses |
| `tx_pause_ctrl_phy` (ebpf2203), `rx_pause_ctrl_phy` (ebpf2201) | Pause frames sent, and whether any reach the sender |
| `outbound_pci_stalled_rd`, `outbound_pci_stalled_wr` (and `_events`) | The NIC waiting on PCIe for reads (outgoing data, such as DAOS writes) or writes (incoming frames to host memory). Rising together with discards points to causes 2 or 3 |
| `port_xmit_data` on the IB device | DAOS bytes sent, in 4-byte words. Gives the DAOS write rate during the run |

Counter names depend on the mlx5 firmware. A counter that `ethtool -S` doesn't list is simply absent from the output.

### Runs, cheapest first

| # | Run | Separates | Outcome → conclusion |
|---|---|---|---|
| 1 | Ask the network team about the switch port toward ebpf2203: does it honor received pause frames (flow control rx on)? Its egress discards and received-pause counters during one failing node-to-node run | 4 | Pause ignored or egress discards: the switch turns every overload into loss. Pause honored with deep buffers would make the problem mostly go away |
| 2 | iperf3 TCP, 8 streams, 30 s into ebpf2203, three times with DAOS idle and three times while ebpf2203 writes to pool `sc26` at 30–40 Gbps with `e2sar_daos_recv --synthetic 2000000 --rate 35 --cont-name load_<s>` (the receiver's own write pattern; [section 7](../results/2026-10-20-synthetic-sink.md#8-the-sink-without-a-sender---synthetic)), with no E2SAR running | 2 vs 1 | TCP retransmits, discards or PCIe stalls rise only with DAOS writing: the shared adapter adds loss. No change: DAOS on the other port is not a factor |
| 3 | Repeat the section 3 pair, three times each: node-to-node, 40 Gbps, `-D 4` vs `-D 16`. Record `port_xmit_data` | 2 vs 1 | Discards only with `-D 16`: same as run 2, with E2SAR traffic. The `-D 4` runs queue events in memory (up to ~4 GB for 100,000 events), so watch memory |
| 4 | Node-to-node, 30 Gbps, `-D 16`, three runs each: `-f 64`, `256`, `1024` at `-P 16`, then `-P 4` and `-P 16` at `-f 256` | 1 | Discards grow with frames per event or with P at the same rate: burst overflow. A fixed DAOS rate in all of these keeps cause 2 constant |
| 5 | Repeat section 6: 1.5 MB events (`-f 4096`), node-to-node, 20 and 40 Gbps, three runs each | The anomaly | Reproduced: burst alignment depends on the pacing interval, and worth a look at how `e2sar_evio_send` paces. Not reproduced: noise from 2 runs |
| 6 | On every run above, compare `outbound_pci_stalled_*` with `rx_discards_phy`. Check the IOMMU mode: `cat /proc/cmdline` (`iommu=pt`?) and `dmesg \| grep -i -e iommu -e dmar` | 3 | Stalls rise with discards even with DAOS idle: host DMA. Then try IOMMU passthrough |
| 7 | A UDP baseline that gets past 20 Gbps: 8 iperf3 client processes on ebpf2201, each pinned (`-A`) to its own core and server port, each `-u -l 8972 -b <total/8>`, total 20–80 Gbps | Path vs E2SAR bursts | Zero loss well above E2SAR's ceiling: the path is fine and E2SAR's bursts are the problem. Loss near the same rate: the port loses evenly paced UDP too |

### What each conclusion would mean for the fix

| If the cause is | Fix |
|---|---|
| Bursts (1) | Pacing inside each event (E2SAR's `--smooth` is too slow), smaller events, or fewer simultaneous Segmenters. Through the LB this is already mostly smoothed |
| The shared adapter (2) | DAOS on a separate adapter or a separate node, or limit the DAOS write rate per receiver and add receivers behind the LB |
| Host DMA (3) | IOMMU passthrough, NIC-local memory for the receive buffers, PCIe settings |
| The switch (4) | Flow control on the switch port, or a switch with deeper buffers. Only this would make node-to-node loss turn into backpressure |

A null sink in `e2sar_daos_recv` (receive and reassemble, skip the DAOS write) would make run 3 cleaner: the same wire traffic with no DAOS traffic at all, and no events queued in memory.
`e2sar_perf -r` is already such a sink, with E2SAR's own sender instead of `e2sar_evio_send`. Section 10 used it and still saw discards. Running `e2sar_evio_send` into `e2sar_perf -r` at the section 7 settings would give the direct comparison.
