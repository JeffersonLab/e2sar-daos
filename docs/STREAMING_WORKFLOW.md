# Streaming workflow: senders → LB → DAOS → live reader

Implemented (2026-10-29): `e2sar_daos_recv` writes the layout of [DAOS_LAYOUT.md](DAOS_LAYOUT.md) and `e2sar_daos_read` reads it as in [DAOS_READER.md](DAOS_READER.md). This page covers the whole workflow, a [walkthrough](#walkthrough---live-rm-streaming-of-an-evio-file) on the ebpf testbed, the senders, and how the receiver settings will be tuned.

## The workflow

```
offsite senders                       JLab
coda-fb / e2sar_evio_send / e2sar_ft  ──UDP──▶ EJFAT LB ──┬──▶ recvA: e2sar_daos_recv (writer slot 0)          ──┐
                                                         └──▶ recvB: e2sar_daos_recv (writer slot 1)          ──┤
                                                                                                                 ▼
                                                                          DAOS container = one run
                                                                          index per UTC second, data arrays per second and slot
                                                                                                                 │
                                                                     e2sar_daos_read (one reader, live)        ◀─┘
                                                                     one file per --interval s, events in order
```

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
| coda-fb | One EVIO-6 record per built time frame (or per group of frames), ~20 KB expected | Frame number, one `dataId` | Today coda-fb is a receiver (LB → ET/files) and sends nothing. It needs a new E2SAR output |
| `e2sar_evio_send` | The frames of an EVIO file (`-F`): each record is one frame, sent as one event | 1…N per Segmenter, `dataId` 4321 + i | Works |
| `e2sar_ft` | One whole file per event (often GBs) | 0, 1, 2, … | Works |
| `e2sar_perf` | Synthetic events of `-l` bytes (1 MiB in the Perlmutter runs) | from 0 | Works |

### What streaming changes in the numbers

1. **Small records hit the event-rate ceiling.** The Reassembler handles ~85k events/s whatever the size. One real-time stream of ~426 B frames is ~15k frames/s, so records must hold many frames once there are several streams or a speedup.
2. **The largest record sets the loss.** Each record leaves the sender as one line-rate burst, and the receiver's port discards grow with burst size ([E2E_BENCHMARK.md](E2E_BENCHMARK.md#diagnosing-the-port-discards)). Smaller records keep the bursts small.
3. **Data age has new stages.** A 256-frame record waits ~16.8 ms in real time to fill, more than the 0.3–0.5 ms node-to-node pipeline. The batch flush (≤ `--flush-us`) and the reader's window (up to `--interval` + 1–3 s, default interval 5 s) come after the DAOS write.
4. **Through the LB, event numbers are LB ticks.** Records that start at different frames on each stream get unrelated ticks, so the same time slice of different streams can go to different receivers. That doesn't matter for storing, since the reader merges all receivers. It does matter for a later step that builds events across streams, which then needs common record boundaries.

## Tuning

Goal: for each scenario, the receiver settings with the highest lossless rate at the lowest CPU, with the live reader keeping up.

**Test path:** sender node → receiver node `e2sar_daos_recv` (DAOS writer) → `e2sar_daos_read` on the sender node, node-to-node, then through the LB once the best settings are known.

| Scenario | Sender (simulated) | What it stresses |
|---|---|---|
| a. coda-fb | `e2sar_evio_send` sending the frames of an EVIO file (~20 KB events), 1 Segmenter (one `dataId`) | Small events: batching, batch fill, reader rate (~15.6k events/s) |
| b. `e2sar_ft`, lossless | One 1.7 GB file, paced below the ~10 Gbps single-event limit; then `e2sar_daos_read --offline` | Large events: split array writes, index after the last piece; restored file identical to the sent one |
| b. `e2sar_ft`, lossy | Several files at once, unpaced (40+ Gbps, events lost in the NIC); then `--offline` | Lost files reported in `_missing.txt`, no partial files; `--live` watermarks while events are lost |
| c. Fastest | `e2sar_perf` with the Perlmutter settings (`--mtu 9000 -l 1048576 --sockets 8 -b 67108864`), `--rate` stepped to 80 Gbps | 1 MiB events (above θ, so not batched). The DAOS side alone at 80 Gbps also with `e2sar_daos_recv --synthetic` |

**Settings swept:**

| Setting | Values |
|---|---|
| Reassembly threads `--threads` | 4, 8, 16, 32 |
| Dequeue threads `--deq` | 2, 4, 8, 16, 32 |
| In flight per thread `--inflight` | 32, 128, 256 |
| `--flush-us` (scenario a) | 1000, 5000, 20000 |

**Recorded per run:** sent and committed Gbps, events lost (reassembly, enqueue, DAOS), mean events and bytes per batch, CPU use of the receiver, DAOS write latency p50/p99, the reader's delay behind the watermark, and, for scenario a, the data age from send to bundle.

The expectations from the design ([DAOS_LAYOUT.md](DAOS_LAYOUT.md#cost-per-event)): with batching, small events need far fewer dequeue threads than today's 16–32, and fewer dequeue threads fill batches better. Reassembly threads stay sized by the receive side. The results go into [E2E_BENCHMARK.md](E2E_BENCHMARK.md) with a recommended setting per scenario.
