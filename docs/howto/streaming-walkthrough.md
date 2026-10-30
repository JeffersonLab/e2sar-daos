---
type: How-To
title: "Walkthrough: --live-rm streaming of an EVIO file"
description: "Stream the frames of an EVIO file node to node on the ebpf testbed, read them live with --live-rm, and check every frame against the file."
tags: [streaming, e2sar_evio_send, e2sar_daos_read, ebpf-testbed]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/STREAMING_WORKFLOW.md", title: "STREAMING_WORKFLOW.md at 0d85d00" }
---

# Walkthrough: `--live-rm` streaming of an EVIO file

The frames of an EVIO file streamed node to node on the ebpf testbed, read live, removed from DAOS as they are read, and checked frame by frame against the file:

```
ebpf2201: e2sar_evio_send ──UDP──▶ ebpf2203: e2sar_daos_recv ──▶ DAOS (sc26) ──▶ ebpf2201: e2sar_daos_read --live-rm ──▶ /scratch
```

**0. Every terminal** (bash, on the node named in the step): the E2SAR environment of [install-e2sar section B](install-e2sar.md#b-without-root-extract-into-your-home-used-on-the-testbed), an e2sar-daos build in `~/daq/e2sar-daos/build`, and the same run name everywhere. Node to node, without the LB, the URI only names the receiver's address and first port:

```bash
cd ~/daq/e2sar-daos
RUNC=stream_1793327563           # the run = the container; pick a new name per run, the same in every terminal
URI="ejfat://useless@10.10.10.10:1234/lb/1?data=129.57.178.86:20600"
```

**1. Writer, ebpf2203.** It creates the container and claims writer slot 0:

```bash
./build/bin/e2sar_daos_recv -u "$URI" --ip 129.57.178.86 --port 20600 --threads 4 --timeout 10000 \
    -b 1073741824 --pool sc26 --cont-name $RUNC --deq 8 --report 5
```
Wait for `Receiving; Ctrl-C to stop`.

**2. Reader, ebpf2201.** One file per 2 s; data is removed from DAOS once it is 2 intervals behind the last file written:

```bash
./build/bin/e2sar_daos_read --pool sc26 --cont $RUNC -o /scratch/$USER/live_$RUNC \
    --live-rm --interval 2 --rm-after 2 --end-wait 5
```
It prints `writer slot 0 found` and waits for data.

**3. Sender, ebpf2201** (another terminal). 4 Segmenters (`dataId` 4321–4324) × 250,000 frames of `frames_thread0_file0010.evio` (mean 407 bytes) at 0.15 Gbps, about 45,000 events/s for 22 s. It reads the whole file first, which takes a few minutes for 1.7 GB from `$HOME`. `--digests` writes the SHA-256 of every frame of the file, for step 5:

```bash
./build/bin/e2sar_evio_send -u "$URI" --port-per-segmenter --mtu 9000 -P 4 -s 1 -n 250000 \
    -F ~/evio-data/frames_thread0_file0010.evio -r 0.15 --digests /scratch/$USER/digests_$RUNC
```
It ends with `Sent: 1000000 events, … 0 errors`. Meanwhile the reader prints `wrote <RUNC>_<W>.evio: …` every 2 s, and the run's objects in DAOS stay few, since `--live-rm` removes what it has read (`daos container list-objects sc26 $RUNC | wc -l` on any node).

**4. Stop the writer.** About 15 s after `Sent:` (the reassembly timeout is 10 s), Ctrl-C on ebpf2203. It prints `Events committed: 1000000` and writes `done/0`. The reader then writes its last file, waits `--end-wait` 5 s for another writer, and ends:
```
… end of run: 12 files, 1000000 events, 394.91 MiB; 0 missing, 834 late, 0 duplicate event numbers
… container stream_1793327563 destroyed
```
`late` counts events that committed after higher-numbered ones and so landed one file later; they are not missing.

**5. Verify, ebpf2201.** Every event in the files must be its frame of the file, byte for byte (event number *n* of any `dataId` is frame (*n* − 1) mod the file's frame count):

```bash
python3 -I bin/e2sar_live_check.py /scratch/$USER/live_$RUNC $RUNC \
    --digests /scratch/$USER/digests_$RUNC --expected 1000000
```
```
12 files, 1000000 events match their frame, 0 bad, 0 duplicates, 0 missing (_missing.txt)
events per dataId: 4321: 250000, 4322: 250000, 4323: 250000, 4324: 250000
expected 1000000, got 1000000
PASS
```
`daos container query sc26 $RUNC` now fails: with `--live-rm` the run is only in the files. Each file is the events of one 2 s window glued in `(evtNum, dataId)` order (EVIO records back to back, without a file header); its `.idx` gives each event's offset and size, and `<RUNC>_manifest.txt` the SHA-256 of each file.

**6. Clean up**, once checked:
```bash
rm -r /scratch/$USER/live_$RUNC /scratch/$USER/digests_$RUNC
```

To keep the run in DAOS, use `--live` instead of `--live-rm` in step 2; then `e2sar_daos_read --pool sc26 --cont $RUNC --fetch` also works after the run, and `--delete` removes it. Through the LB, start the writers with `--withcp` and the instance's `EJFAT_URI` instead ([perlmutter](../sites/perlmutter.md)).
