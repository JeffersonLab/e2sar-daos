---
type: Result
title: "LB 682: Perlmutter EVIO time frames, receiver crashes and losses (2026-11-09)"
description: "Eleven runs on LB 682 (19:21–22:22 UTC): EVIO time frames of 28–45 kB at 3.4–3.8 Gbps (~11k events/s). The DAOS receiver was never the sender's bottleneck (no back-pressure over UDP; 0 enqueue loss, 0 failed writes, ~1/5 of its tested capacity). Losses: 3 MB socket buffers (~10%, fixed with -b 1 GiB); from 20:33, receiver crashes in E2SAR's reassembly when two senders send different events with the same event number and data id (LUX and Perlmutter, then several Perlmutter frame builders); and 26–32% of event numbers that never arrive (a 65.536 µs frame counter, not all frames sent)."
tags: [bug, lb, ejfat, nersc, lux, e2sar, e2sar_daos_recv, ebpf-testbed, loss, crash]
sources:
  - { id: recv, resource: "../reference/e2sar_daos_recv.md", title: "e2sar_daos_recv" }
  - { id: monitor, resource: "../reference/e2sar_daos_monitor.md", title: "e2sar_daos_monitor and its dashboard" }
  - { id: run2, resource: "2026-11-05-201815UTC-lb677-run2-evio.md", title: "LB 677 run 2 (2026-11-05)" }
  - { id: summary, resource: "e2e-summary.md", title: "End-to-end results: summary" }
---

# LB 682: Perlmutter EVIO time frames, receiver crashes and losses (2026-11-09)

EVIO time frames from frame builders at NERSC (Perlmutter) came through LB 682 on the beta control plane (`ejfat-lb.es.net:18008`) to `e2sar_daos_recv` on ebpf2201 and/or ebpf2203, run by `scripts/e2sar_live_pipeline.sh` with a `--live` reader, the DAOS monitor and the EVIO viewer. E2SAR 0.4.0rc1, pool `sc26`. From 19:21 to 20:22 UTC two receivers ran; from 20:24 one. The run data was deleted the same day; the numbers below come from the receiver, monitor, reader and NIC logs that were kept, from packet captures on the receiving nodes, and from a `gdb` backtrace.

## The event frames

| Run (UTC) | Receivers | Events stored | Mean event size | Rate (all receivers) |
|---|---|---|---|---|
| `e2sar_lb682_1794252072` 19:21–19:34 | 2201 + 2203 | 1.85M | 27.7 kB | ~2.4 Gbps |
| `e2sar_lb682_1794252945` 19:35–20:01 | 2201 + 2203 | 15.42M | 41.0 kB | ~3.7 Gbps |
| `e2sar_lb682_1794254618` 20:03–20:22 | 2201 + 2203 | 11.60M | 43.4 kB | ~3.7 Gbps |
| `e2sar_perlmutter_1794259253` 21:20–21:24 | 2203 | 1.99M | 41.2 kB | 3.4–3.8 Gbps |
| `e2sar_perlmutter_1794261744` 22:02–22:05 | 2201 | 0.36M | 45.4 kB | up to 7.9 Gbps (several senders) |

- The sender restarted its numbering at 19:41; the frames grew from ~28 kB to 41–45 kB at the same time.
- Single events ranged from ~250 B to ~46 kB in the captures. The path has a 1500-byte MTU: every full packet was a 1498-byte frame, so a 44 kB event is ~30 packets, and ~11k events/s is ~300k packets/s.
- The event number counts 65.536 µs frames: it advanced 15,251 per second (1/65.536 µs = 15,259/s) while 11,097 events/s arrived (72.8%). It is a clock, not a sequence.
- Compared with LB 677 (2026-11-05: ~10 kB frames at ~1.2 Gbps, [run 2](2026-11-05-201815UTC-lb677-run2-evio.md)), the frames were ~4× larger and the rate ~3× higher.

## Could the DAOS receiver be the sender's bottleneck?

No.

- **No back-pressure path.** E2SAR sends UDP through the LB. A slow receiver can only drop packets (socket buffer, NIC) or queue events; it can't slow the sender. The receiver's fill level goes to the LB control plane (`fillpercent`, 0 all day), which uses it to weigh workers, not to pace senders.
- **The receiver kept up.** In every run: enqueue loss 0, failed DAOS writes 0, DAOS commit throughput equal to ingest (e.g. 1.817 Gbps on each node of `…4618`), watermark lag ~1 s, 2.8–4.7 events per batch.
- **Large headroom.** One receiver was lossless at 20 Gbps through the LB, and the DAOS sink alone commits 92–103 Gbps ([summary](e2e-summary.md)). 3.7 Gbps and ~11k events/s are about 1/5 of that and 1/8 of the Reassembler's ~85k events/s.
- **The 26–32% of event numbers that never arrive are upstream.** No NIC, socket or enqueue drop matched them, and E2SAR loses a partly received event as *reassembly loss*, which stayed at ~0.01%. Whole events were never sent, or were dropped before the LB.
- **The reader is slower than the stream, but it doesn't affect the sender or the receiver.** `e2sar_daos_read --live` wrote ~2 GiB per 5 s window in ~9 s (~1.9 Gbps, the `/scratch` disk) and fell ~1 min behind per minute at 3.7 Gbps. The events stay in DAOS meanwhile; nothing is lost.

## Losses

| Cause | When | Size | Side |
|---|---|---|---|
| Socket buffer too small (3 MB default) | runs before ~19:20 | ~10% of packets (`Udp.RcvbufErrors`), ~1.0M of 10.8M events | ours, fixed: `-b 1073741824` |
| **Receiver crash** (E2SAR reassembly, see below) | every run from 20:33 | everything after the crash until a restart: e.g. 18.4M packets (~650k events) in 61 s for `…9253` | E2SAR bug, triggered by the senders |
| Event numbers never arriving | every run | 26–32% of the numbers | sender side |
| Reassembly loss | every run | ~0.01% (e.g. 1,322 of 11.6M in `…4618`) | network or LB |
| NIC port discards, NIC→UDP bursts | a few runs | 576 port discards in `…9253`; bursts of 3–14k packets at start | ours, small |
| Enqueue loss, failed DAOS writes | — | 0 | — |

## The crash (bug)

From 20:33 UTC every receiver on LB 682 died within seconds to minutes of data arriving, on both nodes, with glibc heap-corruption aborts: `corrupted double-linked list`, `munmap_chunk(): invalid pointer`, `free(): invalid size`, `corrupted size vs. prev_size`, `double free or corruption (out)`, or a silent `SIGSEGV`. The receiver binary hadn't changed since 2026-11-05; the same binary had run 25 min without a crash.

**Where.** Under `gdb` the crashing thread was an E2SAR receive thread, with no DAOS code on the stack:

```
memmove  ←  e2sar::Reassembler::RecvThreadState::_processPacket  (src/e2sarDPReassembler.cpp:359, a 1024-byte packet)
         ←  e2sar::Reassembler::RecvThreadState::_threadBody     (src/e2sarDPReassembler.cpp:498)
```

**Why.** E2SAR reassembles an event by (event number, data id) and sizes its buffer from the event length in the first packet it sees. Packet captures on the receiving node (the RE header of every packet) showed different events with the same event number and data id 1:

| Capture (UTC) | Packets | Event numbers carrying >1 event | Most events under one number |
|---|---|---|---|
| 20:58, LUX and Perlmutter on LB 682 | 893k in 3 s | 137 | 2 (e.g. 5,928 B and 12,408 B) |
| 21:30 | 954k in 3 s | 4 (3 with a ~250 B event) | 2 |
| 22:04, more senders | 1.9M in 4 s | 348 | 7 |
| 22:22, after LUX was removed from LB 682 | 1.05M in 3 s | 244 | 5 |

A packet of the larger event then lands at an offset past the end of the smaller event's buffer, and `memmove` writes past it. Each packet's own header is consistent (offset + length within its event length); the conflict is only between events. Even without a crash, two events sharing a number can be merged into one corrupt event.

**Who.** The LB rewrites the source address to its own (192.188.29.11), so the senders' IPs aren't in the packets. The IP TTL split the traffic into two groups (TTL 47 and 48, ~16–17 hops, both far from JLab), sending at the same rate over the same event numbers, with collisions inside each group too. So at least three sources sent at once. The senders all number events from their own clock (LUX and Perlmutter aren't synchronized) and all use data id 1:

- LUX (198.124.49.31) sent into LB 682 until it was removed from the sender list at ~22:08. A separate LB, 684, was reserved for it.
- After that, the Perlmutter nodes on LB 682's sender list (128.55.83.132, .133, .137, .155, .168) still collided among themselves.

**Fix.**

- *Senders (the cause):* one source per LB, or a distinct data id per frame builder or stream, so no two sources share (event number, data id).
- *E2SAR (the bug):* `_processPacket` should check a packet's offset + length against the buffer it has for that event, and drop or count the packet instead of writing past it. A mismatched event length for a known event should be counted as an error.

## Also found and fixed today

- `e2sar_live_pipeline.sh` started receivers with the 3 MB socket buffer: now `-b 1073741824` by default.
- When the sender restarted its numbering (67.5M → 5.2M at 19:41), the monitor's missing count froze and every new event counted as a duplicate (7.6M in 12 min). `e2sar_daos_monitor` and `e2sar_daos_read` now start a new segment on a jump of more than `--restart-gap` (1,000,000).
- The monitor page counted event-number gaps as losses. With a clock-based event number they aren't: the page shows the receivers' own losses only ([#45](https://github.com/JeffersonLab/e2sar-daos/pull/45)).

## Open

- Which Perlmutter nodes send, and whether frames are skipped by design (empty frames) or dropped (the 26–32%).
- The E2SAR bounds check above, to report to the E2SAR developers with this page.
- Show the LB ID on the monitor page ([#46](https://github.com/JeffersonLab/e2sar-daos/issues/46)).
