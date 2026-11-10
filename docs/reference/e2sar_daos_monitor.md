---
type: Reference
title: "e2sar_daos_monitor and its dashboard"
description: "The read-only DAOS run monitor: incoming and outgoing events, events the receivers lost, and event number gaps every 2 s as JSON lines, every option, the line format, and the live/replay dashboard viz/daos_monitor/serve.py."
tags: [e2sar_daos_monitor, monitoring, dashboard, sc26]
sources:
  - { id: monitor, resource: "../../bin/e2sar_daos_monitor.cpp", title: "bin/e2sar_daos_monitor.cpp" }
  - { id: dashboard, resource: "../../viz/daos_monitor/serve.py", title: "viz/daos_monitor/serve.py and index.html" }
---

# `e2sar_daos_monitor` and its dashboard

`e2sar_daos_monitor` watches one run's DAOS container and writes its state every `--every` seconds (default 2) as one JSON line. [`viz/daos_monitor/serve.py`](../../viz/daos_monitor/serve.py) serves a web page that follows those lines live or replays them. Where it fits in the run: [streaming-workflow, Monitoring](../concepts/streaming-workflow.md#monitoring). Steps for the SC26 booth: [live-monitoring](../howto/live-monitoring.md).

Status: run against DAOS on the ebpf testbed (2026-10-30, 17.4M events at 61k Events/s, 4 dataIds, `--live-rm`): ~8% of one core, one look every 2.02 s, its incoming count within 0.1% of the receiver's committed count. In a second run it flagged 4,330 frames the sender failed to send, within one update, matching the reader's gap list (then `_missing.txt`, now `_gaps.txt`). These numbers came from the event number gaps, which were called *missing* until 2026-11-09.

![The DAOS monitor dashboard during a real run on the ebpf testbed: frames_thread0_file0020.evio, 4 streams × ~15k frames/s (0.2 Gbps), e2sar_daos_read --live-rm --interval 2 --rm-after 2. The 4,330 missing are frames the sender failed to send (sendto: Operation not permitted, 1,082–1,083 per stream); the receiver lost none, and the reader's _missing.txt lists the same numbers](../images/sc26_daos_monitor.png)

## What it counts

It reads only the container (the catalog, `seconds`, `index[T]`), never the data arrays or the reader's files. It changes nothing, so writers and readers run the same with or without it.

| Quantity | Meaning |
|---|---|
| Incoming | Events newly committed: index entries not seen before |
| Outgoing | Events that left DAOS: their index second was removed, by `e2sar_daos_read --live-rm`. Always 0 with `--live` |
| In DAOS | Incoming − outgoing, in events and bytes, and the number of index seconds present |
| Missing | Events the receivers lost, summed over the writer slots, from the counters each receiver writes to the catalog once a second (`wm/<slot>`, [daos-layout](../concepts/daos-layout.md#catalog)): lost in reassembly (some of the event's packets didn't arrive within `--timeout`), lost in enqueue (E2SAR's queue was full), and failed DAOS writes. Never goes down. An event of which no packet reached a receiver isn't counted: no receiver saw it. A writer from a receiver built before 2026-11-09 has no counters and is reported as *without counts* |
| Gaps | Per `dataId`, event numbers absent for more than `--grace` seconds (5) while a higher number of that `dataId` is committed. Only a hint of loss: a sender need not number its events one after another, and then gaps are not losses. Two counts: *so far* (cumulative: each number counted once, when its gap passes `--grace`; never goes down) and *still open* (drops when a late event fills its gap) |
| Pending | In a gap for less than `--grace`: another writer may still commit them |
| Sender restarts | Per `dataId`, jumps of more than `--restart-gap` event numbers (1,000,000), up or down: a sender that restarted its numbering. The numbers after a jump are a new segment with its own gaps, not one huge gap, and not duplicates. Late events of an older segment still fill its gaps. The strip and `first`/`max` show the newest segment |
| Late | Numbers that were absent and then arrived |
| Duplicates | Numbers committed twice for one `dataId` |

`Gaps` uses the same rule as the reader's `<run>_gaps.txt`, but it is counted as events commit, without waiting for a window to be written. `Missing` and the reader's end-of-run `receivers lost` line read the same counters.

### How it reads

Every `--every` seconds:

1. Read the writers' slots, watermarks (with their loss counters) and `done` entries from the catalog.
2. List `seconds`. A second it counted before that is now gone has left DAOS: its events become outgoing.
3. For each second still present and not final, list `index[T]` and read the batch tables it hasn't counted yet. A second is final once every writer's index watermark is past it; after that it is never read again.
4. Add each new event to its `dataId`'s gap tracker (sorted by `(evtNum, dataId)`), and write the line.

The cost per event is one 32-byte batch-table entry, read once, plus one `seconds` listing and one `index[T]` listing per second still open. Not yet measured on DAOS.

## Usage

```bash
e2sar_daos_monitor --pool sc26 --cont <run> -o /scratch/$USER/<run>_monitor.jsonl
python3 viz/daos_monitor/serve.py /scratch/$USER/<run>_monitor.jsonl          # http://localhost:8766
```

- Start it before or after the writers. If the container doesn't exist yet, it waits (`waiting for container …`).
- It ends on Ctrl-C, or when the container is destroyed: at the end of a `--live-rm` run it writes `{"type":"end","why":"container destroyed"}` and exits.
- `-o` is appended to, not overwritten. Use one file per run so a replay shows one run.
- Each line is written whole or not at all. While the disk is full the monitor skips lines (logged once, and again when it writes) and keeps running; the page shows a gap. On start it drops a cut-off last line left by a crash.
- Each line also prints to stdout in short form:

```
1793280042.123 in 12630000 events (+468000) | out 9720000 (+532000) | in DAOS 2910000 events, 56908.2 MiB, 6 s | missing 580 (reassembly 577, enqueue 0, failed 3) | gaps 18 so far (11 open), pending 4
```

| Option | Default | Meaning |
|---|---|---|
| `--pool`, `--cont` | required | Pool and the run's container label |
| `-o, --out` | `<cont>_monitor.jsonl` | JSON lines file, appended |
| `--every` | 2 | Seconds between looks at the container |
| `--grace` | 5 | Seconds an event number may be absent before it counts as a gap |
| `--restart-gap` | 1000000 | A jump of more than this many event numbers is a sender restart (a new segment), not a gap |
| `--span` | 0 | Event numbers per `dataId` in the strip. 0: `--strip-seconds` of events at the fastest `dataId`'s rate, rounded up to a power of two × `--bins`; it only grows, so it settles after the first updates with data |
| `--strip-seconds` | 32 | With `--span 0`: about this many seconds of events in the strip |
| `--bins` | 600 | Bins per strip |

## The line format

One JSON object per line. `type` is `tick`, or `end` once.

| Field | Content |
|---|---|
| `tick`, `t`, `dt` | Line number, UTC time (s), seconds since the previous line |
| `run`, `start` | Container label, run start (UTC s) from the run record |
| `sources` | Where the run's data comes from (each writer's `source`, deduplicated, e.g. `["LB 682"]`); empty for receivers built before 2026-11-10 |
| `writers[]` | `slot`, `source`, `done`, `wm_index`, `wm_data`, `lag` (now − `wm_index`, s), `events`, `bytes` committed by that slot; `lost`, `reassembly_loss`, `enqueue_loss`, `failed` when the receiver writes its counters |
| `in`, `out` | `events`, `bytes` (run totals), `d_events`, `d_bytes` (since the previous line); `out.d_seconds`: index seconds removed since the previous line |
| `daos` | `events`, `bytes` in DAOS, `seconds` present, `first`, `last` second |
| `missing` | Events the receivers lost: `total`, `reassembly`, `enqueue`, `failed`, summed over the writers with counters; `writers_unknown`: writers without counters |
| `gaps` | `total` (cumulative), `events` (still open), `pending`, `late`, `dups`, `restarts`, summed over the `dataId`s |
| `strip` | `span` event numbers shown per `dataId`, `bins`, `width` = event numbers per bin |
| `streams[]` | Per `dataId`: `id`, `events`, `bytes`, `d_events`, `first`, `max`, `gaps` (open), `gaps_total` (cumulative), `restarts`, `pending`, `late`, `dups`, `strip_lo` (first event number of the strip) and `strip` |

`strip` is the state of event numbers `strip_lo … strip_lo + span − 1` in `bins` bins, run-length encoded, e.g. `o420d176g1d3` = 420 bins left DAOS, 176 in DAOS, 1 gap, 3 in DAOS. A bin shows the first of these states that any of its event numbers has, so a bin with any stored event is `d` or `o`, and `g` or `p` only when none of its events is stored:

| Letter | State |
|---|---|
| `d` | In DAOS |
| `o` | Left DAOS |
| `g` | Gap (`m` in files from before 2026-11-09, which ranked gaps first: one absent number marked its bin) |
| `p` | Pending |
| `u` | Not seen: not committed yet, or before the first event the monitor saw |

A line with 18 `dataId`s is about 4 KB, or about 7 MB per hour at one line per 2 s.

Files written before 2026-11-09 have no receiver counters: their `missing` object and the streams' `missing`/`missing_total` are the gap counts. The page reads them as gaps and shows *Missing* as not known.

## The dashboard: `viz/daos_monitor/serve.py`

Standard library only (Python ≥ 3.8). It reads the JSON lines file as it grows and serves the page on `127.0.0.1` (default port 8766). The page polls once a second. If the run name in the file changes (a new run appended), the page starts over.

| Part | Shows |
|---|---|
| Header | Live or replay, the run's `sources` (e.g. *LB 682*, *node-to-node*), the run name, writers and `dataId`s |
| Tiles | Incoming and outgoing Gbps (large) and Events/s, data in DAOS, *Missing* (lost by the receivers: reassembly, enqueue, failed). Event number gaps aren't shown: with a clock-based event number a sender skips numbers by design (LB 682: ~27% of 65.536 µs frame numbers); they stay in the JSON lines (`gaps`) and the log line |
| Event numbers per `dataId` | One row per `dataId`, the newest number at the right: in DAOS (blue), left DAOS (aqua), not seen (gray, also bins whose numbers are all absent). Each row ends with that `dataId`'s Events/s; hover for the event range of a bin |
| Gbps | Incoming and outgoing Gbps (hover: Events/s too), the last 5 minutes before the frame shown. Incoming: over the last 3 updates (~6 s). Outgoing: events per second of data removed, over the last 5 updates. A plain per-update rate would flicker: the reader removes one `--interval` window at a time, at times that jitter against the monitor's 2 s looks, so a look can catch 0 or 2 windows (seen in the first real run, between 0 and 125k Events/s; on 2026-11-10 the bursts were the reader removing one window at a time while it fell behind 20 Gbps). A red ▲ on the time axis marks an update in which the receivers lost events (in older files: event numbers went into gaps); hover for how many and the total so far. Loss counts share no axis with Gbps, so they are marks, not a line |
| Data in DAOS | GiB committed and not yet removed, mean of the last 3 updates, the last 5 minutes |
| Writers | Each slot's watermark and lag (*behind* if more than 5 s), and the events it lost: reassembly / enqueue / failed |

**Live** shows the newest line. **Replay** plays the file from its start: Play/Pause (space), speed 1–32×, a slider, and ←/→ for one frame.

| Option | Default | Meaning |
|---|---|---|
| `JSONL` | | The monitor's `-o` file (with `--demo`: the file to write) |
| `--port` | 8766 | Port on 127.0.0.1 |
| `--demo` | off | Write made-up lines (18 `dataId`s at ~15k Events/s each, two writers, a `--live-rm --interval 2 --rm-after 2` reader, the beam off for 10 s every 90 s, a few lost events, counted both as reassembly loss and as gaps) to `JSONL`, for trying the page without DAOS |
| `--backfill` | 0 | With `--demo`: first write this many seconds of past lines at once, so the charts start full |
| `--every` | 2 | With `--demo`: seconds per line |
