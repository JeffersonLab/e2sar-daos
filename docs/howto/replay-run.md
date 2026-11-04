---
type: How-To
title: "Replay a run in the DAOS monitor and the EVIO live viewer"
description: "After a run, play it again in both pages without DAOS, receivers or a sender: the DAOS monitor from its JSON lines file, the EVIO viewer from the reader's --live files or from one EVIO file with live_replay.py."
tags: [monitoring, replay, e2sar_daos_monitor, evio-interactive-view, ebpf-testbed]
sources:
  - { id: monitoring, resource: "live-monitoring.md", title: "Live monitoring of a run from a laptop" }
  - { id: monitor, resource: "../reference/e2sar_daos_monitor.md", title: "e2sar_daos_monitor and its dashboard" }
  - { id: viewer, resource: "https://code.jlab.org/xmei/evio-interactive-view", title: "evio-interactive-view: serve.py --live, live_replay.py" }
  - { id: run, resource: "../results/2026-11-04-nersc-lb675-evio.md", title: "The run used to test this page" }
---

# Replay a run in the DAOS monitor and the EVIO live viewer

The two pages of [live monitoring](live-monitoring.md) can show a finished run again. Neither replay reads DAOS, so the container may already be gone, and nothing has to run but the two web servers.

| Page | Replays from | Kept after a run? |
|---|---|---|
| DAOS monitor (port 8766) | the monitor's JSON lines file (`-o` of `e2sar_daos_monitor`) | yes, the reader and `--live-rm` don't touch it |
| EVIO live viewer (port 8765) | the reader's `--live` files and their manifest, or any EVIO v6 file | only if the run's output folder (or a joined EVIO file) was kept |

Status: run on ebpf2203 on 2026-11-04 with the run of [2026-11-04-nersc-lb675-evio](../results/2026-11-04-nersc-lb675-evio.md) (2,587 monitor lines, 615 bundles, a 9.7 GB joined file): each server served the replayed data. The pages themselves were not opened in a browser for this test.

## 1. The DAOS monitor

```bash
RUNC=e2sar_lb675_1793817098            # the run = the container name
cd ~/daq/e2sar-daos
python3 viz/daos_monitor/serve.py /scratch/$USER/${RUNC}_monitor.jsonl           # port 8766
```

In the page pick **Replay**: it plays the file from the start at 1–32× (space: play/pause, ←/→: one frame, or drag the slider). **Live** shows the last line, which for a finished run is the end. `serve.py` needs only Python ≥ 3.8, so a copy of the `.jsonl` file replays on a laptop too.

## 2. The EVIO live viewer

`serve.py --live DIR` shows the newest bundle in DIR. To replay, fill a new folder with bundles at the run's pace. Two ways:

### a. From the run's own files (same windows and event numbers)

Copy the bundles in manifest order, one per `--interval` seconds, and keep only the newest few:

```bash
SRC=/scratch/$USER/live_$RUNC            # the reader's -o DIR
REPLAY=/tmp/$USER-replay_$RUNC; mkdir -p $REPLAY
I=5; SPEED=1                              # I: the run's --interval; SPEED: 2 = twice as fast
awk '{print $2}' $SRC/${RUNC}_manifest.txt | while read f; do
  cp $SRC/$f $SRC/${f%.evio}.idx $REPLAY/
  ls -t $REPLAY/*.evio | tail -n +4 | while read o; do rm -f "$o" "${o%.evio}.idx"; done   # keep 3
  sleep $(awk "BEGIN{print $I/$SPEED}")
done
```

and in a second terminal:

```bash
cd ~/daq/evio-interactive-view && .venv/bin/python serve.py --live $REPLAY --no-browser   # port 8765
```

To start later in the run, skip lines of the manifest (`awk 'NR>=100 {print $2}' …`). The bundles are the ones the reader wrote, so the event numbers, `dataId`s and gaps are the run's.

### b. From one EVIO file (`live_replay.py`)

Any EVIO v6 file of time frames, for example a run joined into one file ([2026-11-04-nersc-lb675-evio, One EVIO file from the bundles](../results/2026-11-04-nersc-lb675-evio.md#one-evio-file-from-the-bundles)):

```bash
cd ~/daq/evio-interactive-view
.venv/bin/python live_replay.py /path/to/run.evio $REPLAY --interval 5 --keep 3   # --speed S, --loop
.venv/bin/python serve.py --live $REPLAY --no-browser                            # other terminal
```

It sorts the file's frames by frame number and cuts windows of `--interval` seconds of frames (65.536 µs each). It first indexes the whole file: 84 s for the 9.7 GB, 46.9M-frame file on ebpf2203, then one bundle per `--interval / --speed` seconds. The bundles are named after the current time, not the run's, and carry `dataId` 4321; the event number is the frame number. Needs the viewer's `.venv` (numpy).

Use **a** to see the run as it was; use **b** when only an EVIO file is left, or on a laptop with a copy of the file.

## 3. On the laptop

Forward both ports as in [live-monitoring, step 4](live-monitoring.md#4-the-laptop):

```bash
ssh -N -L 8766:localhost:8766 -L 8765:localhost:8765 $USER@<node>
```

then open http://localhost:8766 and http://localhost:8765. Both servers listen on 127.0.0.1 only, so anyone with an account on the node can open the pages with their own tunnel.

## 4. Afterwards

Ctrl-C the servers and the copy loop (or `live_replay.py`), then `rm -rf $REPLAY`. The run's own files are not changed by either replay.
