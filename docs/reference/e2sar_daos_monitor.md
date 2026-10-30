---
type: Reference
title: "e2sar_daos_monitor and its dashboard"
description: "The read-only DAOS run monitor: incoming, outgoing and missing events every 2 s as JSON lines, every option, the line format, and the live/replay dashboard viz/daos_monitor/serve.py."
tags: [e2sar_daos_monitor, monitoring, dashboard, sc26]
sources:
  - { id: monitor, resource: "../../bin/e2sar_daos_monitor.cpp", title: "bin/e2sar_daos_monitor.cpp" }
  - { id: dashboard, resource: "../../viz/daos_monitor/serve.py", title: "viz/daos_monitor/serve.py and index.html" }
status: draft
---

# `e2sar_daos_monitor` and its dashboard

`e2sar_daos_monitor` watches one run's DAOS container and writes its state every `--every` seconds (default 2) as one JSON line. [`viz/daos_monitor/serve.py`](../../viz/daos_monitor/serve.py) serves a web page that follows those lines live or replays them. Where it fits in the run: [streaming-workflow, Monitoring](../concepts/streaming-workflow.md#monitoring). Steps for the SC26 booth: [live-monitoring](../howto/live-monitoring.md).

Status: the dashboard was tested with `serve.py --demo`; the monitor has not yet run against DAOS.

![The DAOS monitor dashboard, demo data: 18 dataIds, 2 writers, --live-rm reader](../images/sc26_daos_monitor.png)

## What it counts

It reads only the container (the catalog, `seconds`, `index[T]`), never the data arrays or the reader's files. It changes nothing, so writers and readers run the same with or without it.

| Quantity | Meaning |
|---|---|
| Incoming | Events newly committed: index entries not seen before |
| Outgoing | Events that left DAOS: their index second was removed, by `e2sar_daos_read --live-rm`. Always 0 with `--live` |
| In DAOS | Incoming − outgoing, in events and bytes, and the number of index seconds present |
| Missing | Per `dataId`, event numbers absent for more than `--grace` seconds (5) while a higher number of that `dataId` is committed. Two counts: *so far* (cumulative: each number counted once, when it first goes missing; never goes down) and *still open* (drops when a late event fills its gap) |
| Pending | Absent for less than `--grace`: another writer may still commit them |
| Late | Numbers that were absent and then arrived |
| Duplicates | Numbers committed twice for one `dataId` |

`Missing` uses the same rule as the reader's `<run>_missing.txt` (gaps per `dataId`), but it is counted as events commit, without waiting for a window to be written.

### How it reads

Every `--every` seconds:

1. Read the writers' slots, watermarks and `done` entries from the catalog.
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
- Each line also prints to stdout in short form:

```
1793280042.123 in 12630000 events (+468000) | out 9720000 (+532000) | in DAOS 2910000 events, 56908.2 MiB, 6 s | missing 18 so far (11 open), pending 4
```

| Option | Default | Meaning |
|---|---|---|
| `--pool`, `--cont` | required | Pool and the run's container label |
| `-o, --out` | `<cont>_monitor.jsonl` | JSON lines file, appended |
| `--every` | 2 | Seconds between looks at the container |
| `--grace` | 5 | Seconds an event number may be absent before it counts as missing |
| `--span` | 0 | Event numbers per `dataId` in the strip. 0: `--strip-seconds` of events at the fastest `dataId`'s rate, rounded up to a power of two × `--bins`, fixed once known |
| `--strip-seconds` | 32 | With `--span 0`: about this many seconds of events in the strip |
| `--bins` | 600 | Bins per strip |

## The line format

One JSON object per line. `type` is `tick`, or `end` once.

| Field | Content |
|---|---|
| `tick`, `t`, `dt` | Line number, UTC time (s), seconds since the previous line |
| `run`, `start` | Container label, run start (UTC s) from the run record |
| `writers[]` | `slot`, `done`, `wm_index`, `wm_data`, `lag` (now − `wm_index`, s), `events`, `bytes` committed by that slot |
| `in`, `out` | `events`, `bytes` (run totals), `d_events`, `d_bytes` (since the previous line) |
| `daos` | `events`, `bytes` in DAOS, `seconds` present, `first`, `last` second |
| `missing` | `total` (cumulative), `events` (still open), `pending`, `late`, `dups`, summed over the `dataId`s |
| `strip` | `span` event numbers shown per `dataId`, `bins`, `width` = event numbers per bin |
| `streams[]` | Per `dataId`: `id`, `events`, `bytes`, `d_events`, `first`, `max`, `missing` (open), `missing_total` (cumulative), `pending`, `late`, `dups`, `strip_lo` (first event number of the strip) and `strip` |

`strip` is the state of event numbers `strip_lo … strip_lo + span − 1` in `bins` bins, run-length encoded, e.g. `o420d176m1d3` = 420 bins left DAOS, 176 in DAOS, 1 missing, 3 in DAOS. A bin shows the most important state among its event numbers, in this order:

| Letter | State |
|---|---|
| `m` | Missing: at least one event number of the bin |
| `p` | Pending |
| `d` | In DAOS |
| `o` | Left DAOS |
| `u` | Not seen: not committed yet, or before the first event the monitor saw |

A line with 18 `dataId`s is about 4 KB, or about 7 MB per hour at one line per 2 s.

## The dashboard: `viz/daos_monitor/serve.py`

Standard library only (Python ≥ 3.8). It reads the JSON lines file as it grows and serves the page on `127.0.0.1` (default port 8766). The page polls once a second. If the run name in the file changes (a new run appended), the page starts over.

| Part | Shows |
|---|---|
| Tiles | Incoming and outgoing Events/s and Gbps, data in DAOS, missing so far (with still open, arrived late, pending, duplicates) |
| Event numbers per `dataId` | One row per `dataId`, the newest number at the right: in DAOS (blue), left DAOS (aqua), missing (red, full height, marked ▲), pending (yellow), not seen (gray). Each row ends with that `dataId`'s missing count so far and Events/s; hover for the event range of a bin |
| Events/s | Incoming and outgoing, the last 5 minutes before the frame shown. A red ▲ on the time axis marks an update in which event numbers went missing; hover for how many and the total so far. Missing counts share no axis with Events/s, so they are marks, not a line |
| Data in DAOS | GiB committed and not yet removed, the last 5 minutes |
| Writers | Each slot's watermark and lag: *behind* if more than 5 s |

**Live** shows the newest line. **Replay** plays the file from its start: Play/Pause (space), speed 1–32×, a slider, and ←/→ for one frame.

| Option | Default | Meaning |
|---|---|---|
| `JSONL` | | The monitor's `-o` file (with `--demo`: the file to write) |
| `--port` | 8766 | Port on 127.0.0.1 |
| `--demo` | off | Write made-up lines (18 `dataId`s at ~15k Events/s each, two writers, a `--live-rm --interval 2 --rm-after 2` reader, the beam off for 10 s every 90 s, a few lost events) to `JSONL`, for trying the page without DAOS |
| `--backfill` | 0 | With `--demo`: first write this many seconds of past lines at once, so the charts start full |
| `--every` | 2 | With `--demo`: seconds per line |
