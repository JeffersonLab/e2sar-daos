---
type: How-To
title: "Live monitoring of a run from a laptop (SC26 booth)"
description: "Run the DAOS monitor and the EVIO live viewer next to e2sar_daos_read, forward both pages through one SSH connection, and show them on a laptop; rehearse without DAOS with the demo modes."
tags: [monitoring, e2sar_daos_monitor, evio-interactive-view, ssh, sc26]
sources:
  - { id: monitor, resource: "../reference/e2sar_daos_monitor.md", title: "e2sar_daos_monitor and its dashboard" }
  - { id: viewer, resource: "https://code.jlab.org/xmei/evio-interactive-view/-/tree/sc26-live-viz", title: "evio-interactive-view, branch sc26-live-viz: serve.py --live, live_replay.py, live_demo.py" }
---

# Live monitoring of a run from a laptop (SC26 booth)

Two pages for one run, on a laptop that reaches the reader node only through SSH: the DAOS monitor (port 8766) and the EVIO live viewer (port 8765). What each shows and where it runs: [streaming-workflow, Monitoring](../concepts/streaming-workflow.md#monitoring).

Status: run on the ebpf testbed on 2026-10-30 (receiver on ebpf2203; monitor, reader, sender and both pages on ebpf2201), viewed from a Mac through the tunnel of step 4.

## 0. Once: get the code on the reader node

```bash
cd ~/daq/e2sar-daos && git pull && meson compile -C build      # builds build/bin/e2sar_daos_monitor
cd ~/daq && git clone -b sc26-live-viz git@code.jlab.org:xmei/evio-interactive-view.git
```

`viz/daos_monitor/serve.py` needs only Python ≥ 3.8. The viewer's `serve.py` needs only the standard library too; its `live_demo.py` needs numpy.

## 1. Start the run as usual

Receivers, then the reader, as in [streaming-workflow](../concepts/streaming-workflow.md#the-workflow). For the booth, one bundle per 2 s and data removed from DAOS soon after:

```bash
RUNC=sc26_1793500000                  # the run = the container; the same in every terminal
OUT=/scratch/$USER/live_$RUNC
./build/bin/e2sar_daos_read --pool sc26 --cont $RUNC -o $OUT --live-rm --interval 2 --rm-after 2
```

## 2. The DAOS monitor, on the reader node

Two terminals (or `tmux`). The monitor can start before the receivers; it waits for the container.

```bash
./build/bin/e2sar_daos_monitor --pool sc26 --cont $RUNC -o /scratch/$USER/${RUNC}_monitor.jsonl
python3 viz/daos_monitor/serve.py /scratch/$USER/${RUNC}_monitor.jsonl          # port 8766
```

## 3. The EVIO live viewer, on the reader node

```bash
mkdir -p $OUT                          # the viewer needs the folder to exist
cd ~/daq/evio-interactive-view && python3 serve.py --live $OUT --no-browser   # port 8765
```

## 4. The laptop

One SSH connection forwards both ports (add `-J <gateway>` if the reader node is behind a login host):

```bash
ssh -N -L 8766:localhost:8766 -L 8765:localhost:8765 $USER@<reader-node>
```

Then open:

| Page | URL | For the booth |
|---|---|---|
| DAOS monitor | http://localhost:8766 | **Live** follows the run. **Replay** plays the run from the start at 1–32× (space: play/pause, ←/→: one frame) |
| EVIO live viewer | http://localhost:8765 | Opens on the **Live** tab: channels × time in frame, all crates, rates. **Pause** holds a bundle; click a channel to open it in **Plots** |

The viewer pulls one sample per bundle through the tunnel: 32 MiB per 2 s (~130 Mbit/s) by default. Pick 16 MiB in the page if the booth link is slow, or 128 MiB on a fast one.

## 5. After the run

The monitor exits once a `--live-rm` run's container is destroyed (otherwise Ctrl-C). The JSON lines file stays, and `serve.py` replays it at any time, without DAOS:

```bash
python3 viz/daos_monitor/serve.py /scratch/$USER/${RUNC}_monitor.jsonl
```

## Rehearse without DAOS

On any machine, including a laptop:

```bash
# the DAOS monitor page, with made-up lines: 18 dataIds, 2 writers, a --live-rm reader
python3 viz/daos_monitor/serve.py /tmp/demo_monitor.jsonl --demo --backfill 300   # http://localhost:8766, 5 min of history

# the viewer with real data: an EVIO file replayed as 2 s bundles in real time
cd evio-interactive-view
python live_replay.py /path/to/frames_thread0_file0000.part000.evio /tmp/live --loop
python serve.py --live /tmp/live                                                  # http://localhost:8765

# or made-up bundles at SC26 scale: 18 crates × 16 boards × 16 channels, ~230 MB each
python live_demo.py /tmp/live
```
