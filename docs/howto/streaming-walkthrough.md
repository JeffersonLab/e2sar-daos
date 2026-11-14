---
type: How-To
title: "Walkthrough: a live run on two ebpf nodes, from LB to EVIO file"
description: "Every step of one run on the ebpf testbed: a valid LB instance URI, NIC monitors, receivers on ebpf2201 and ebpf2203, the --live-rm reader, DAOS monitor and EVIO live viewer on ebpf2203, the pages forwarded to a laptop, an EVIO file sent through the LB, stopping, checking every frame, and one bundle turned into an EVIO file for someone else."
tags: [streaming, e2sar_evio_send, e2sar_daos_recv, e2sar_daos_read, e2sar_daos_monitor, evio-interactive-view, lb, ebpf-testbed]
status: draft
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/STREAMING_WORKFLOW.md", title: "STREAMING_WORKFLOW.md at 0d85d00" }
  - { id: pipeline, resource: "../../scripts/e2sar_live_pipeline.sh", title: "scripts/e2sar_live_pipeline.sh: the same layout in one command" }
  - { id: lbtest, resource: "lb-test.md", title: "Reserving and checking an LB instance" }
  - { id: monitoring, resource: "live-monitoring.md", title: "Live monitoring of a run from a laptop" }
---

# Walkthrough: a live run on two ebpf nodes, from LB to EVIO file

One run with every process started by hand, in the order the [streaming workflow](../concepts/streaming-workflow.md#the-workflow) requires: the frames of an EVIO file sent through an EJFAT LB to two receivers, read live into files, watched on two pages from a laptop, checked frame by frame, and one bundle turned into an EVIO file to hand to someone else.

```
                                     ┌─▶ ebpf2201: e2sar_daos_recv (slot 0) ─┐
ebpf2201: e2sar_evio_send ──▶ LB ────┤                                       ├─▶ DAOS (sc-demo) ─▶ ebpf2203: e2sar_daos_read --live-rm ─▶ /scratch/epsdaq/live_<run>/*.dat
                                     └─▶ ebpf2203: e2sar_daos_recv (slot 1) ─┘        │                                │
                                                                                     ebpf2203: e2sar_daos_monitor :8766   ebpf2203: EVIO live viewer :8765
                                                                                                    └──── ssh -L ────▶ laptop ◀────┘
```

Status: draft. The [workflow script](#the-quick-path-on-the-nodes-the-workflow-script) ran on 2026-11-14 through LB 686 into sc-demo: `start` on ebpf2201 then ebpf2203, 400,000 frames of `frames_thread0_file0010.evio` sent from ebpf2201 with `e2sar_evio_send -c` (the LB split them 199,370 / 200,630), `status`, `list`, `evio 2` (90,489 records), `stop` on ebpf2201 then ebpf2203: 6 bundles, 400,000 events, 0 lost, 0 gaps, 0 duplicates, container destroyed. The manual steps have not yet run as one sequence. The pieces have run: this layout through LB 686 into sc-demo with `scripts/e2sar_live_pipeline.sh` (2026-11-12, [results](../results/2026-11-12-004630UTC-lb686-live-rm.md)), `e2sar_evio_send -c` from a testbed node through LB 682 ([results](../results/2026-11-10-lb682-rate-sweep-ramp.md)), and the frame check of step 9 node to node (2026-10-30).

| Terminal | Node | Runs | Started in step |
|---|---|---|---|
| A | ebpf2201 | NIC monitor, then the receiver (slot 0) | 2, 3 |
| B | ebpf2203 | NIC monitor, then the receiver (slot 1) | 2, 3 |
| C | ebpf2203 | DAOS monitor | 4 |
| D | ebpf2203 | DAOS monitor page (:8766) | 4 |
| E | ebpf2203 | Reader | 5 |
| F | ebpf2203 | EVIO live viewer (:8765) | 6 |
| G | laptop | Port forward | 7 |
| H | ebpf2201 | Sender | 8 |

`tmux` on each node keeps them apart and survives a dropped SSH connection. Or let one command do steps 1–6 and another step 9, right after step 0: [on the nodes, one by one](#the-quick-path-on-the-nodes-the-workflow-script), or [from a laptop](#the-quick-path-from-a-laptop-the-pipeline-script).

## 0. Once: code, data and an LB instance

On a node (`$HOME` is shared between the ebpf nodes, so once is enough):

```bash
cd ~/daq/e2sar-daos && git pull && meson compile -C build    # e2sar_daos_recv, _read, _monitor, e2sar_evio_send
cd ~/daq && git clone git@code.jlab.org:xmei/evio-interactive-view.git   # the EVIO live viewer (standard library only)
ls -l ~/evio-data/frames_thread0_file0010.evio                # the file to send (1.7 GB, EVIO v6)
```

The E2SAR tools and libraries come from `~/.bashrc` ([install-e2sar section B](install-e2sar.md#b-without-root-extract-into-your-home-used-on-the-testbed)): it sets `E2SAR_BIN` and the library path.

**An LB instance URI.** Senders and receivers find the LB through `EJFAT_URI`, the *instance* URI that `lbadm --reserve` prints. It holds everything: the token, the control plane address, the LB id (`/lb/<id>`), the sync address and the data addresses. Keep it in a private file in `$HOME` and never in the repo. To reserve one: [lb-test step 3](lb-test.md). Then check it in a bash shell:

```bash
bash                                          # the nodes' login shell is tcsh
source ~/.bashrc; source ~/e2sar-lb686.env   # EJFAT_URI: the instance URI of LB 686
echo "$EJFAT_URI" | cut -d@ -f2               # without the token: ejfat-lb.es.net:18008/lb/<id>?sync=...&data=...
$E2SAR_BIN/lbadm --status                     # answers with the LB id and its (empty) worker list
# not an LB: the node-to-node placeholder (invalid; only data= is real), on which lbadm --status fails
# export EJFAT_URI="ejfat://useless@10.10.10.10:1234/lb/1?data=$MY_IP:20700"
```

A URI is only valid while its reservation lasts. Having a well-formed `EJFAT_URI` in the shell doesn't mean the LB still exists; `lbadm --status` is the test. Everything below needs it to pass:

| `EJFAT_URI` | `lbadm --status` | Do |
|---|---|---|
| Valid: a reserved, unexpired instance | The LB id and its workers (none yet) | Go on |
| Empty | `Error in parsing URI` | `source ~/e2sar-lb686.env` in this shell |
| The node-to-node placeholder `ejfat://useless@10.10.10.10:1234/…` | `failed to connect ... 10.10.10.10:1234` | It is not an LB: use a fresh shell, or source the instance file again |
| An instance that was freed, or whose reservation ran out (`lbadm --reserve -d`) | Fails: the LB doesn't know the id or the token any more | Reserve a new one ([lb-test step 3](lb-test.md)) and write it to the instance file |
| Valid, but the CA isn't installed or the certificate doesn't match | A TLS or certificate error | Add `-v` (`--novalidate`) to every `lbadm`, `e2sar_daos_recv` and `e2sar_evio_send` command below |

A receiver or sender started with an invalid URI fails at registration (`-c`) and sends or receives nothing, so check before step 3, not after.

## The quick path on the nodes: the workflow script

[`scripts/e2sar_workflow.sh`](../../scripts/e2sar_workflow.sh) is run on the ebpf nodes themselves, one node at a time: log in to a node, run a command, log in to the next. Each node starts or stops only its own part, so the nodes never SSH to each other (they can't without a password); they find the run through files in the shared `$HOME` (`~/e2sar_run/workflow/`). On every node it runs the NIC monitor and a receiver through the LB; on the reader node (`READER`, default ebpf2203) also the reader, the DAOS monitor, its page and the EVIO viewer. Defaults: `ENV_FILE=e2sar-lb686.env`, `POOL=sc-demo`, `OUT=/scratch/epsdaq`, the reader with `--live-rm --interval 2 --rm-after 2 --min-free 50`, pages on 8766 and 8765.

Once, a short name for it (the login shell is tcsh; in bash, `alias workflow=...`):

```bash
alias workflow ~/daq/e2sar-daos/scripts/e2sar_workflow.sh
```

A run, from start to stop:

| Where | Command | What it does |
|---|---|---|
| ebpf2201 | `workflow start` | Checks the LB (step 0): `lbadm --status` on `EJFAT_URI` from `~/e2sar-lb686.env`, and refuses an invalid one. Opens a new run named after the LB id, `e2sar_lb686_<UTC epoch s>` (step 1), then the NIC monitor and the receiver (steps 2, 3) |
| ebpf2203 | `workflow start` | Joins that run: NIC monitor and receiver, then the reader, the DAOS monitor and its page, the EVIO viewer (steps 2–6). Prints the laptop command for step 7 |
| laptop | `ssh -N -L 8766:localhost:8766 -L 8765:localhost:8765 ebpf2203` | Step 7 (`workflow forward` prints it); open http://localhost:8766 and http://localhost:8765 |
| a sender | step 8 | Send through the LB |
| either | `workflow status` | The run, each node's receiver and its last progress line, the LB's workers; on ebpf2203 also the reader, monitor, pages, bundles and free space |
| ebpf2203 | `workflow list` | The run's bundles (`.dat` files), numbered |
| ebpf2203 | `workflow evio 5` | Bundle 5 as an EVIO file, `/scratch/epsdaq/<bundle>.evio` (step 12): group-readable, with its SHA-256 and the `scp` line to copy it off. Also `workflow evio FILE.dat`, or `workflow evio all` for the whole run; an optional last argument names the output. Works during the run and after it |
| ebpf2201 | `workflow stop` | Its receiver (drains, deregisters from the LB) and NIC monitor; logs to `~/e2sar_run/workflow/<run>/` |
| ebpf2203 | `workflow stop` | Its receiver and NIC monitor, then waits for the reader to finish (it ends once every receiver is done), then the monitor and both pages (step 9). It refuses while ebpf2201's receiver still runs; `workflow stop force` stops the reader anyway |

Start ebpf2201 first and stop it first: the reader node's `stop` waits for the reader, and the reader ends only once every receiver is done. `start` on a node where the run's receiver is already up refuses. A node that starts while no other node's receiver is up opens a new run. Only one workflow per LB at a time: two runs registered with the same LB would split its events. The checks of steps 10–13 are by hand, with `RUNC=$(cat ~/e2sar_run/workflow/current)`.

## The quick path from a laptop: the pipeline script

[`scripts/e2sar_live_pipeline.sh`](../../scripts/e2sar_live_pipeline.sh) does steps 1–6 with `start` and step 9 with `stop`, for every node at once. It runs on a machine that can SSH to both nodes without a password prompt: a laptop with `ControlMaster` connections, opened by logging in to each node once (on Windows, from WSL; the nodes can't SSH to each other). It sends nothing (step 8), forwards nothing (step 7), and checks and converts nothing (steps 10–13); do those as below.

```bash
cd ~/daq/e2sar-daos                      # a checkout on the laptop; the nodes use their own ~/daq/e2sar-daos
export ENV_FILE=e2sar-lb686.env POOL=sc-demo LOGDIR=e2sar_run/walk \
       READ_MODE=--live-rm READ_ARGS="--min-free 50 --interval 2 --rm-after 2"
scripts/e2sar_live_pipeline.sh start     # steps 0 (check) and 1–6
ssh -O forward -L 8766:localhost:8766 -L 8765:localhost:8765 ebpf2203      # step 7
```

What `start` does with these settings:

- **Checks the URI (step 0)** on ebpf2203: sources `~/$ENV_FILE` and runs `lbadm --status`. If that fails (no URI, the node-to-node placeholder, a freed or expired instance), it stops before starting anything: `~/e2sar-lb686.env on ebpf2203: no EJFAT_URI, or lbadm --status fails on it (not a valid LB instance)`.
- **Names the run (step 1)** from the LB id in the URI, `e2sar_lb686_<UTC epoch s>`, and writes it to `~/e2sar_run/walk/current`. Set `LABEL` to name it `LABEL_<UTC epoch s>` instead. It refuses if the current run still has a receiver up.
- **Steps 2–6**, with the defaults `RECV` (receivers on ebpf2201, then ebpf2203, each on its own data address), `READER=ebpf2203`, `OUT=/scratch/epsdaq`, `PORT=20700`, `MON_PORT=8766`, `VIEW_PORT=8765`, and `RECV_ARGS="--threads 4 --deq 16 --report 10 --log-idle -b 1073741824"`. The logs go to `~/e2sar_run/walk/` while the run is up. The viewer runs with `~/daq/evio-interactive-view/.venv/bin/python`, so that checkout needs a `.venv`.

It ends with each receiver's `listening on …:20700`, the reader's folder and ports, and the LB's slots per worker:

```
[…] start e2sar_lb686_1794640304
[ebpf2201] e2sar_daos_recv listening on …:20700
[ebpf2203] e2sar_daos_recv listening on …:20700
[ebpf2203] reader -> /scratch/epsdaq/live_e2sar_lb686_1794640304, monitor page :8766, viewer :8765
[LB] name=ebpf2201.jlab.org	slotsassigned=…
[…] running e2sar_lb686_1794640304
```

Then send (step 8) from a terminal on ebpf2201 set up as in step 1, which picks up the run name from the same file. While it runs, and to end it:

```bash
scripts/e2sar_live_pipeline.sh status    # what runs where, LB workers, events per receiver, reader files, free space
scripts/e2sar_live_pipeline.sh stop      # step 9: receivers (they drain), NIC monitors, the reader (waits up to 60 s
                                         # for it to finish), the monitor and both pages; logs to ~/e2sar_run/walk/<run>/
```

Then steps 10–13. The pages are stopped by `stop`; for step 11 start them again by hand. `watch`, for waiting on a remote sender that starts at an unknown time: [live-monitoring](live-monitoring.md#waiting-for-a-sender).

## 1. Every terminal on a node

The same run name everywhere: `e2sar_lb<LB id>_<UTC epoch s>`, the LB id taken from the URI, so the run's container, logs and files all say which LB the data came through. Nothing is typed by hand. The first terminal makes the name once and writes it to a file in the shared `$HOME`; every other terminal, on either node, reads it from there. Don't run `date` in each terminal: each would get a different second, and so a different run.

**Once, in the first terminal** (after the `source` lines below; skip this with the pipeline script, whose `start` writes the file):

```bash
mkdir -p ~/e2sar_run/walk
LB=$(echo "$EJFAT_URI" | grep -oE '/lb/[0-9]+' | cut -d/ -f3)
echo e2sar_lb${LB}_$(date -u +%s) > ~/e2sar_run/walk/current   # e.g. e2sar_lb686_1794700000
```

**In every terminal on ebpf2201 and ebpf2203**, the first one included:

```bash
bash
source ~/.bashrc; source ~/e2sar-lb686.env
cd ~/daq/e2sar-daos
RUNC=$(cat ~/e2sar_run/walk/current); echo $RUNC $MY_IP   # the run = the DAOS container; the same in every terminal
L=~/e2sar_run/walk/$RUNC; mkdir -p $L          # logs and NIC samples (shared $HOME); the script's stop puts its logs here too
OUT=/scratch/epsdaq/live_$RUNC                 # the reader's files, on ebpf2203's /scratch
```

`MY_IP` comes from `~/.bashrc` (`export MY_IP=$(hostname -I | awk '{print $1}')`): this node's own data address, the one receivers listen on and the LB lets senders send from. No address is typed anywhere below. If `echo $MY_IP` prints nothing, the shell didn't read `~/.bashrc`.

Overwrite `~/e2sar_run/walk/current` only to start a new run, and after the old one has stopped: a terminal opened later picks up whatever name is in the file. `scripts/utc.py $RUNC` turns the name back into a date.

## 2. NIC monitors, ebpf2201 and ebpf2203 (terminals A and B)

Before the receivers, every time: without the 1 s samples, a burst of missing events can't be placed afterwards (wire, NIC, kernel or socket):

```bash
scripts/e2sar_nicmon.sh start $L               # writes $L/nic_before_<host>.txt and samples into $L/nic_<host>.log
```

## 3. Receivers, ebpf2201 then ebpf2203 (terminals A and B)

ebpf2201 first: it creates the container and the run record and takes writer slot 0. Start ebpf2203 only after ebpf2201 prints `Receiving; Ctrl-C to stop`; it joins the container as slot 1. With `-c` each registers with the LB as a worker under its hostname.

The same command in both terminals, A on ebpf2201 first, then B on ebpf2203. `--ip $MY_IP` is the node's own data address:

```bash
./build/bin/e2sar_daos_recv -c --ip $MY_IP --port 20700 --pool sc-demo --cont-name $RUNC \
    --threads 4 --deq 16 --report 10 --log-idle -b 1073741824 2>&1 | tee $L/recv_$(hostname -s).log
```

Keep `-b 1073741824` (a 1 GiB socket buffer): with the 3 MB default, LB 682 lost ~10% of packets at the socket at ~1.5 Gbps. Both workers must have slots before anything is sent:

```bash
$E2SAR_BIN/lbadm --status | grep -oE 'name=[^,]+|slotsassigned=[0-9]+' | paste - -   # ebpf2201 and ebpf2203, slots > 0
```

## 4. DAOS monitor and its page, ebpf2203 (terminals C and D)

```bash
# C
./build/bin/e2sar_daos_monitor --pool sc-demo --cont $RUNC -o /scratch/epsdaq/${RUNC}_monitor.jsonl
# D
python3 viz/daos_monitor/serve.py /scratch/epsdaq/${RUNC}_monitor.jsonl --port 8766
```

The monitor writes one JSON line per 2 s: events coming in, events leaving DAOS, events lost per receiver. The page serves them on `127.0.0.1:8766` only.

## 5. Reader, ebpf2203 (terminal E)

After the receivers (a reader started before the container exists exits with an error). One file per 2 s; data is removed from DAOS 2 intervals behind the last file written, and the container is destroyed at the end of the run:

```bash
./build/bin/e2sar_daos_read --pool sc-demo --cont $RUNC -o $OUT \
    --live-rm --interval 2 --rm-after 2 --end-wait 5 --min-free 50 2>&1 | tee $L/read.log
```

It prints `writer slot 0 found`, `writer slot 1 found`, and waits for data. `--min-free 50` pauses it (nothing lost) when `/scratch` gets below 50 GiB free. To keep the run in DAOS after it ends, use `--live` instead of `--live-rm --rm-after 2` (then delete it yourself in step 12).

## 6. EVIO live viewer, ebpf2203 (terminal F)

It shows the newest bundle the reader wrote, decoded: hits per channel × time in frame, per-crate maps, rates.

```bash
mkdir -p $OUT                                  # the viewer needs the folder to exist
cd ~/daq/evio-interactive-view && python3 serve.py --live $OUT --port 8765 --no-browser
```

## 7. Port forward to the laptop (terminal G)

Both pages listen on `127.0.0.1` of ebpf2203 only. One SSH connection forwards both; from outside JLab it goes through the login hosts:

```bash
ssh -N -L 8766:localhost:8766 -L 8765:localhost:8765 -J scilogin.jlab.org,tblogin.jlab.org $USER@ebpf2203
```

With a `Host ebpf2203` entry in `~/.ssh/config` that has `ProxyJump scilogin,tblogin`, this is `ssh -N -L 8766:localhost:8766 -L 8765:localhost:8765 ebpf2203`. If a connection is already open with `ControlMaster`, add the forward to it instead: `ssh -O forward -L 8766:localhost:8766 -L 8765:localhost:8765 ebpf2203`. `Port forwarding failed` means another connection holds the port: stop it, or cancel its forward (`ssh -O cancel -L ... <host>`).

Then open, on the laptop:

| Page | URL | What to look at |
|---|---|---|
| DAOS monitor | http://localhost:8766 | The header names the LB and the run (`LB 686 · e2sar_lb686_… · 2 writers`). **Live** follows the run; **Replay** plays it from the start at 1–32× |
| EVIO live viewer | http://localhost:8765 | **Live** tab: the newest 2 s bundle. **Pause** holds one; click a channel to open it in **Plots** |

Both pages are empty until step 8 sends.

## 8. Send, ebpf2201 (terminal H)

4 Segmenters (`dataId` 4321–4324) × 250,000 frames of the file at 0.15 Gbps, about 45,000 events/s for 22 s, through the LB, which spreads the events over both receivers. `-c` adds `--ip` to the LB's sender list and removes it at the end. It reads the whole file first, which takes a few minutes for 1.7 GB from `$HOME`. `--digests` writes the SHA-256 of every frame of the file, for the check in step 10; it goes to `$L`, so ebpf2203 can read it.

```bash
./build/bin/e2sar_evio_send -c --ip $MY_IP --mtu 9000 -P 4 -s 1 -n 250000 \
    -F ~/evio-data/frames_thread0_file0010.evio -r 0.15 --digests $L/digests 2>&1 | tee $L/send.log
```

It ends with `Sent: 1000000 events, … 0 errors`. Meanwhile: both receivers' progress lines count events, the reader prints `wrote <RUNC>_<W>.dat: …` every 2 s, the monitor page shows incoming events on both writers, and the viewer moves to each new bundle. For a longer run raise `-n` (events per Segmenter; ~4 min at `-n 2500000`) and use the same total in step 10's `--expected`.

## 9. Stop the workflow

In this order: the sender has already ended; then the receivers, the NIC monitors, the reader, the monitor, the pages, the forward.

1. **Receivers** (A and B), about 15 s after `Sent:` (the reassembly timeout is 10 s): Ctrl-C on each. Each deregisters from the LB, drains its writes, prints `Events committed: …` and its loss counters, and writes `done/<slot>`.
2. **NIC monitors** (A and B):
   ```bash
   scripts/e2sar_nicmon.sh stop $L               # writes $L/nic_after_<host>.txt
   scripts/e2sar_nicmon.sh report $L             # drop counters over the run, and seconds in which packets didn't reach UDP
   ```
3. **Reader** (E) ends by itself once both slots are done and no new writer appeared for `--end-wait` 5 s: it writes the last bundle, then
   ```
   … end of run: 12 files, 1000000 events, 394.91 MiB; receivers lost 0 (reassembly 0, enqueue 0, failed 0); 0 in event number gaps, … late, 0 duplicate event numbers
   … container e2sar_lb686_1794700000 destroyed
   ```
   `late` counts events that committed after higher-numbered ones and so landed one file later; they are not missing.
4. **DAOS monitor** (C) exits once the container is destroyed (with `--live`, Ctrl-C). The JSON lines file stays.
5. **Pages** (D and F) and **the forward** (G): Ctrl-C, or keep them for step 11.

Check that nothing is left: `$E2SAR_BIN/lbadm --status` lists no workers (a killed receiver drops off after ~10 s), `pgrep -fa "$RUNC"` on each node prints nothing, and `daos cont list sc-demo | grep $RUNC` prints nothing.

## 10. Check every frame, ebpf2203

Every event in the files must be its frame of the file, byte for byte (event number *n* of any `dataId` is frame (*n* − 1) mod the file's frame count):

```bash
python3 -I bin/e2sar_live_check.py $OUT $RUNC --digests $L/digests --expected 1000000
```
```
12 files, 1000000 events match their frame, 0 bad, 0 duplicates, 0 missing (_missing.txt)
events per dataId: 4321: 250000, 4322: 250000, 4323: 250000, 4324: 250000
expected 1000000, got 1000000
PASS
```

With `--live-rm` the run is now only in the files. Each `<RUNC>_<W>.dat` holds the events of one 2 s window glued in `(evtNum, dataId)` order (EVIO records back to back, without a file header); its `.idx` gives each event's offset and size, and `<RUNC>_manifest.txt` lists every file with its SHA-256.

## 11. Replay the run

- **DAOS monitor:** `python3 viz/daos_monitor/serve.py /scratch/epsdaq/${RUNC}_monitor.jsonl --port 8766` on ebpf2203, forward as in step 7, and pick **Replay**. The `.jsonl` file is small; a copy replays on a laptop too (Python ≥ 3.8, no DAOS).
- **EVIO viewer:** refill a folder with the run's bundles at the run's pace and point `serve.py --live` at it: [replay-run](replay-run.md#2-the-evio-live-viewer).

## 12. One bundle as an EVIO file for someone else

A `.dat` bundle is EVIO v6 records without the 14-word EVIO file header, so it is not an EVIO file yet. [`scripts/evio/evio_concat.py`](../../scripts/evio/evio_concat.py) adds the header (with the record count) and sets the last-record bit on the last record only; only its output is named `.evio`. On ebpf2203:

```bash
awk '{print NR, $2, $5}' $OUT/${RUNC}_manifest.txt | head   # line, file, events: pick a bundle
F=$(awk 'NR==5{print $2}' $OUT/${RUNC}_manifest.txt)        # e.g. the 5th: e2sar_lb686_1794700000_1794700128.dat
python3 scripts/evio/evio_concat.py /scratch/epsdaq/${F%.dat}.evio $OUT/$F   # prints the records copied
sha256sum /scratch/epsdaq/${F%.dat}.evio                    # send this along with the file
```

The whole run as one file instead: `cd $OUT && python3 ~/daq/e2sar-daos/scripts/evio/evio_concat.py /scratch/epsdaq/$RUNC.evio $(awk '{print $2}' ${RUNC}_manifest.txt)`. evio-interactive-view opens files under 2 GiB; split a longer one with its `evio_split.py`.

To hand it over:

- **Someone in group `epsci` on ebpf2203:** `/scratch/epsdaq` is group-readable (`epsci`), so the path is enough. Run `chmod g+r` on the file if your umask left it private.
- **Anyone else:** copy it off the node. From the laptop: `scp -J scilogin.jlab.org,tblogin.jlab.org $USER@ebpf2203:/scratch/epsdaq/<file>.evio .`, then share it as you share other files. Or copy it to `$HOME` (shared by the ebpf nodes; mind its quota) or another JLab disk the person can read.

## 13. Clean up

Once checked and handed over:

```bash
rm -r $OUT /scratch/epsdaq/${RUNC}_monitor.jsonl $L/digests      # ebpf2203; keep the .jsonl if you want to replay the monitor
rm /scratch/epsdaq/${F%.dat}.evio                               # once the other person has it
daos cont list sc-demo | grep $RUNC                             # nothing with --live-rm; with --live: daos cont destroy sc-demo $RUNC
```

The logs and NIC samples in `$L` are small; keep them with the run. Don't free the LB unless you reserved it for this run alone (`lbadm --free`, [lb-test](lb-test.md)).

## Node to node, one receiver

Without an LB, a sender reaches one receiver only, at the address in the URI's `data=`. The URI is a placeholder, not a valid LB URI (`lbadm --status` fails on it): only `data=` is real. The receiver writes its own `$MY_IP` to the shared `$HOME`, and the sender builds the URI from it, so no address is typed:

```bash
# receiver node (B, ebpf2203): no -c, so no LB
echo $MY_IP > $L/recv_ip
./build/bin/e2sar_daos_recv --ip $MY_IP --port 20700 --pool sc-demo --cont-name $RUNC \
    --threads 4 --deq 16 --report 10 --log-idle -b 1073741824 2>&1 | tee $L/recv_$(hostname -s).log

# sender node (H, ebpf2201), after the receiver prints "Receiving;"
export EJFAT_URI="ejfat://useless@10.10.10.10:1234/lb/1?data=$(cat $L/recv_ip):20700"
./build/bin/e2sar_evio_send --port-per-segmenter --mtu 9000 -P 4 -s 1 -n 250000 \
    -F ~/evio-data/frames_thread0_file0010.evio -r 0.15 --digests $L/digests 2>&1 | tee $L/send.log
```

`--port-per-segmenter` sends Segmenter i to port 20700 + i, one receive thread each, so the receiver needs `--threads` ≥ `-P`. Skip the NIC monitor and receiver on ebpf2201; the rest is the same. Use a separate shell for it: the placeholder overrides the instance URI, and `source ~/e2sar-lb686.env` brings that back.
