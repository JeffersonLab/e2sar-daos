# Streaming workflow: senders → LB → DAOS → live reader

Implemented (2026-10-29): `e2sar_daos_recv` writes the layout of [DAOS_LAYOUT.md](DAOS_LAYOUT.md) and `e2sar_daos_read` reads it as in [DAOS_READER.md](DAOS_READER.md). This page covers the whole workflow, a [walkthrough](#walkthrough---live-rm-streaming-of-an-evio-file) on the ebpf testbed, the senders, and how the receiver settings will be tuned.

## The workflow

![Streaming workflow: senders, EJFAT LB, two receivers writing one DAOS container, one live reader](images/streaming_workflow_demo-sc26.svg)

The receivers (DAOS writers) and the reader communicate only through the DAOS container: no sockets or shared files. The one rule outside the container is the order: receivers first, then the reader ([writer–reader contract](DAOS_LAYOUT.md#writerreader-contract)). The sequence:

1. **Receivers.** Start `e2sar_daos_recv` on recvA and recvB with the same `--cont-name`. The first creates the container and the run record; each claims a writer slot. With `--withcp` they register with the LB. Receivers must be up before the senders start, or the LB has nowhere to send.
2. **Reader.** Start `e2sar_daos_read --pool <pool> --cont <same name> -o DIR --live` (or `--live-rm` to remove data from DAOS as it reads; a mode is required, see [DAOS_READER.md](DAOS_READER.md)) on the reader node, after the receivers have started (the first one creates the container; a reader started before that exits with an error). It can start before the senders, during the run, or after it (it catches up).
3. **Senders.** Start sending. The LB sends every event number to one receiver.

While the run goes on:

- Each receiver batches small events and writes them asynchronously. A batch becomes visible when its index entry is written.
- Each receiver writes a watermark once a second. Second T is complete when every receiver's watermark is past T.
- The reader checks the container every second and writes one file `DIR/<run>_<W>.evio` per `--interval` window (default 5 s), its events glued back to back in `(evtNum, dataId)` order, about 1–3 s after the window ends. With `--live-rm` it removes objects from DAOS once they are `--rm-after` intervals (default 10) behind the last file written.

At the end: stop the senders, then the receivers (Ctrl-C). Each writes its final statistics (`done/<slot>`). The reader sees that in the container, writes the last bundles and exits once every receiver is done and no new one has appeared for `--end-wait` seconds.

### What the reader guarantees

| | |
|---|---|
| Complete windows | A bundle is written only after every receiver has committed everything for each second of its window |
| Order | Strict within a bundle. Across bundles close to event-number order; events that come out late are counted |
| Restart | The manifest's last bundle is the restart point. A bundle's content depends only on DAOS, so a rerun writes identical files |
| File transfers (`e2sar_ft`) | With `--offline` (after the transfer): one file per `(evtNum, dataId)`, byte for byte the sent file, with its SHA-256 printed; the run stays in DAOS until `--delete`. A file with any lost packet is dropped by E2SAR and only appears in `<run>_missing.txt` |
| Lost events | Gaps per `dataId` go to `<run>_missing.txt`. A receiver that dies is reported, and the reader continues without it after a timeout |

## Walkthrough: `--live-rm` streaming of an EVIO file

The frames of an EVIO file streamed node to node on the ebpf testbed, read live, removed from DAOS as they are read, and checked frame by frame against the file:

```
ebpf2201: e2sar_evio_send ──UDP──▶ ebpf2203: e2sar_daos_recv ──▶ DAOS (sc26) ──▶ ebpf2201: e2sar_daos_read --live-rm ──▶ /scratch
```

**0. Every terminal** (bash, on the node named in the step): the E2SAR environment of [E2SAR_INSTALL.md section B](E2SAR_INSTALL.md#b-without-root-extract-into-your-home-used-on-the-testbed), an e2sar-daos build in `~/daq/e2sar-daos/build`, and the same run name everywhere. Node to node, without the LB, the URI only names the receiver's address and first port:

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

To keep the run in DAOS, use `--live` instead of `--live-rm` in step 2; then `e2sar_daos_read --pool sc26 --cont $RUNC --fetch` also works after the run, and `--delete` removes it. Through the LB, start the writers with `--withcp` and the instance's `EJFAT_URI` instead ([LCF2JLAB_GUIDE.md](LCF2JLAB_GUIDE.md)).

## Senders

| Sender | Events | Event number | Status |
|---|---|---|---|
| coda-fb | One EVIO-6 record per built time frame (or per group of frames), ~20 KB expected | Frame number, one `dataId` |  |
| `e2sar_evio_send` | The frames of an EVIO file (`-F`): each record is one frame, sent as one event | 1…N per Segmenter, `dataId` 4321 + i | Works |
| `e2sar_ft` | One whole file per event (often GBs) | 0, 1, 2, … | Works |
| `e2sar_perf` | Synthetic events of `-l` bytes (1 MiB in the Perlmutter runs) | from 0 | Works |

### What streaming changes in the numbers

1. **Small records hit the event-rate ceiling.** The Reassembler handles ~85k events/s whatever the size. One real-time stream of ~426 B frames is ~15k frames/s, so records must hold many frames once there are several streams or a speedup.
2. **The largest record sets the loss.** Each record leaves the sender as one line-rate burst, and the receiver's port discards grow with burst size ([E2E_BENCHMARK.md](E2E_BENCHMARK.md#diagnosing-the-port-discards)). Smaller records keep the bursts small.
3. **Data age has new stages.** A 256-frame record waits ~16.8 ms in real time to fill, more than the 0.3–0.5 ms node-to-node pipeline. The batch flush (≤ `--flush-us`) and the reader's window (up to `--interval` + 1–3 s, default interval 5 s) come after the DAOS write.
4. **Through the LB, event numbers are LB ticks.** Records that start at different frames on each stream get unrelated ticks, so the same time slice of different streams can go to different receivers. That doesn't matter for storing, since the reader merges all receivers. It does matter for a later step that builds events across streams, which then needs common record boundaries.

## Tuning

The receiver settings swept on 2026-10-29 with [`scripts/e2sar_layout_sweep.py`](../scripts/e2sar_layout_sweep.py): reassembly threads (`--threads`), dequeue threads (`--deq`), DAOS operations in flight per dequeue thread (`--inflight`) and, for small events, `--flush-us`. One receiver, `e2sar_daos_recv` on ebpf2203 into pool `sc26`; senders on ebpf2201.

**Node to node, not through the LB.** Without the LB nothing spreads one sender stream over several receiver ports, so each stream goes to its own port, which one reassembly thread serves: the reassembly threads equal the number of sender streams. Each run had its own container, destroyed after it. Loss is split into reassembly loss (`Lost in reassembly`, with the NIC's `rx_discards_phy`), enqueue loss (dequeue threads behind) and DAOS failures; the last two were 0 in every run.

### Results

*Italics* mark the recommended setting in each table.

**a. coda-fb-like: 20 KB events at 2.5 Gbps** (`e2sar_perf -l 20480`, one stream, 1 reassembly thread, `--inflight 32`; 300,000 events):

| `--deq` | `--flush-us` | Lossless | Events per batch | Latency p50 / p99 | Receiver CPU |
|---|---|---|---|---|---|
| *1* | *5000* | *yes* | *51* | *2.4 / 3.9 ms* | *1.5 cores* |
| *2* | *5000* | *yes* | *41* | *3.1 / 5.8 ms* | *1.6 cores* |
| 4 | 5000 | yes | 24 | 3.1 / 6.3 ms | 1.8 cores |
| 8 | 5000 | yes | 11 | 3.4 / 6.8 ms | 3.7 cores |
| 1–4 | 1000 | yes | 7–17 | 0.9 / 1.5 ms | 1.5–2.1 cores |
| 8 | 1000 | 2 events lost in reassembly | 3.6 | 1.2 / 2.4 ms | 3.9 cores |
| 1–8 | 20000 | yes | 39–51 | 2.4–11.5 / 3.9–23 ms | 1.5–3.4 cores |

**b. `e2sar_ft`, 2 GB EVIO files** (one stream, 1 reassembly thread, `--timeout 10000`):

| Run | Result |
|---|---|
| *1 file at 5 Gbps*; `--deq` 1–8, `--inflight` 16–128, `--max-write` 16M or 64M (*`--deq 4`, `--inflight 128`*) | *Lossless in every run.* Once reassembled, the 2 GB event is in DAOS 158–179 ms later (~100 Gbps); the settings make little difference |
| 4 files at 20 Gbps | All 4 lost in reassembly (1,605 NIC port discards) |
| 4 files unpaced | All 4 lost in reassembly (336 NIC port discards) |

**c1. The DAOS side alone: `--synthetic` 1 MiB events, unpaced** (30,000 events):

| `--deq` | `--inflight` 32 | `--inflight` 128 | Latency p50 |
|---|---|---|---|
| *4* | 170 Gbps | *182 Gbps* | *~1 ms* |
| 8 | 174 Gbps | 186 Gbps | 9–38 ms |
| 16 | 176 Gbps | 167 Gbps | 19–84 ms |
| 32 | 138 Gbps | 153 Gbps | 42–185 ms |

**c2. 1 MiB events, 8 `e2sar_perf` streams** (`--sockets 8` each, 8 reassembly threads, `--inflight 128`, 15 s per run):

| Total rate | Committed (*`--deq 8`*) | Events lost, `--deq` *8* / 16 / 32 | NIC port discards (`--deq 8`) | Receiver CPU (`--deq 8`) |
|---|---|---|---|---|
| 20 Gbps | 20.0 Gbps | *3* / 5 / 5 of 35,760 | 33 | 19 cores |
| 40 Gbps | 39.9 Gbps | *3* / 5 / 10 of 71,520 | 12 | 19 cores |
| 60 Gbps | 59.9 Gbps | *35* / 318 / 20 of 107,288 | 404 | 19 cores |
| 80 Gbps | 79.8 Gbps | *35* / 46 / 164 of 143,048 | 412 | 20 cores |

**c3. As c2 with 2 sender sockets per stream** (`e2sar_perf --sockets 2`), 8 or 16 streams and reassembly threads. Events not committed, of 71,520 (40 Gbps), 107,288 (60) or 143,048 (80):

| Reassembly threads | Total rate | `--deq 8 --inflight 32` | `--deq 8 --inflight 128` | `--deq 16 --inflight 32` | `--deq 16 --inflight 128` | Receiver CPU |
|---|---|---|---|---|---|---|
| 8 | 40 Gbps | *0* | 7 | 3 | 481 | *12 cores* (`--deq 8 --inflight 32`), 19–20 otherwise |
| 8 | 60 Gbps | *15* | 23 | 24 | 10 | *19–21 cores* |
| 8 | 80 Gbps | *15* | 30 | 36 | 10,825 ¹ | *20–21 cores* |
| 16 | 40 Gbps | 10 | 7 | 11 | 7 | 37–38 cores |
| 16 | 60 Gbps | 27 | 596 ² | 1,386 | 121 | 37–38 cores |
| 16 | 80 Gbps | 52 | 25 | 56 | 48 | 38–39 cores |

All lost events were reassembly losses with NIC port discards, except: ¹ 556,250 `rx_out_of_buffer` drops in that one run, while other tests ran on the sender node; ² 46 reassembly losses, the other 550 not accounted for by the receiver. Both are treated as outliers.

### Analysis

1. **DAOS is not the limit.** The layout commits 170–186 Gbps of 1 MiB events with 4–8 dequeue threads (c1), and through the network every event that reached the receiver was committed, up to 80 Gbps (c2). No run had enqueue loss or a failed DAOS write.
2. **The loss is in the NIC, as before.** Every lost event was a reassembly loss, and the NIC's port discards (`rx_discards_phy`) rose with it ([E2E_BENCHMARK.md](E2E_BENCHMARK.md#diagnosing-the-port-discards)). From 60 Gbps it is ~0.03 % of 1 MiB events with 8 dequeue threads.
3. **Fewer dequeue threads are better.** More threads cost CPU (19 → 23 cores from 8 to 32 at 80 Gbps) and, at 80 Gbps, more loss (35 → 164 events), likely because they compete with the reassembly threads for cores. For small events they also split the stream into smaller batches: 51 events per batch with one thread, 11 with eight, for the same 2.5 Gbps.
4. **Batching makes small events cheap.** 15,600 events/s of 20 KB take 1.5 cores with 51 events per 1 MiB write. `--flush-us` trades latency for batch size: 1 ms gives sub-millisecond latency with 7–17 events per write; 20 ms adds latency and no larger batches once they are full.
5. **Fewer sender sockets, and no more reassembly threads than streams.** With 2 sockets per stream instead of 8, the loss at 80 Gbps halved (15 events instead of 35, c3 vs c2), and 40 Gbps was lossless at 12 cores. 16 reassembly threads (and 16 streams) doubled the CPU, to 37–39 cores, without losing less. `--inflight 32` did as well as 128.
6. **A large file is all or nothing.** A 2 GB `e2sar_ft` event is lost if any of its ~240,000 packets is, so one stream must be paced (5 Gbps was lossless) and the reassembly timeout must be longer than its transfer (3.4 s at 5 Gbps). Writing it to DAOS takes ~0.17 s, whatever the settings.

### Recommended settings

| Scenario | `--threads` | `--deq` | `--inflight` | Other |
|---|---|---|---|---|
| a. Small events (~20 KB), a few Gbps | 1 per stream | 1–2 | 32 | `--flush-us 5000`; `1000` for latency below 1 ms |
| b. Files with `e2sar_ft` | 1 per stream | 4 | 128 | Pace the sender (≤ 5 Gbps per stream); `--timeout` longer than one file's transfer |
| c. 1 MiB events, up to 80 Gbps | 1 per stream (port) | 8 | 32 | 8 streams, 2 sender sockets each (`--sockets 2`) |
