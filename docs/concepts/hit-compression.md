---
type: Concept
title: "Compressing hit frames for the WAN: ideas and a first look"
description: "Prototyping ideas, not a result: how much the EVIO time frames of LB 677 run 1 (97% fADC hit words) shrink, measured on 20,000 frames: lz4 1.00x, zstd 1.17-1.30x, Parquet 1.22-1.65x, and 2.32x for a hit-aware encoding (time step within a port bank, channel, charge) at ~480 MB/s per core. With so few header words Parquet gains little in size, unlike the cosmics file. Compression doesn't touch the losses seen so far (periodic bursts, not bandwidth), but would cut the WAN rate of the full 18-crate SRO by about 2.3x."
status: draft
tags: [compression, parquet, hit-words, evio, fadc250, wan, lb, sro, evio-interactive-view]
sources:
  - { id: run1, resource: "../results/2026-11-05-194730UTC-lb677-run1-evio.md", title: "LB 677 run 1: EVIO time frames from NERSC to two receivers" }
  - { id: run2, resource: "../results/2026-11-05-201815UTC-lb677-run2-evio.md", title: "LB 677 run 2: EVIO time frames from NERSC, losses and stopping a receiver" }
  - { id: viewer, resource: "https://code.jlab.org/xmei/evio-interactive-view/-/blob/main/README.md", title: "evio-interactive-view README: evio_decode.py (EVIO to Parquet) and the hit-word layout" }
---

# Compressing hit frames for the WAN: ideas and a first look

**Draft: prototyping ideas, not a result.** Nothing here is built. The numbers come from one quick measurement in Python on 20,000 frames of one file (2026-11-05), to see which ideas are worth trying.

The frames of [LB 677 run 1](../results/2026-11-05-194730UTC-lb677-run1-evio.md) are 97.2% fADC hit words. Earlier EVIO files were mostly headers: in the EC5/EC6 cosmics file only 22% of the bytes were hits, and converting it to Parquet with zstd made it 4.7x smaller, because the headers and empty banks went away. This page asks the same question for the LB 677 data, and whether compression could ease the WAN link between facilities (the narrow middle of the sender → LB → receiver path) once the full 18-crate streaming readout (SRO) runs.

## The quick measurement

- **Data:** the first 20,000 frames (194.1 MB) of `/scratch/epsdaq/live_e2sar_lb677_1793905825/e2sar_lb677_1793905825_1793908285.evio`, the 49th of run 1's 224 `--live` files. 9,703 bytes and 2,360 hit words per frame, hit words 97.3% of the bytes; one ROC (12), 13 payload banks per frame, 1.3% of them empty.
- **Script:** [`scripts/evio/evio_hit_compression.py`](../../scripts/evio/evio_hit_compression.py) `FILE 20000`, run on ebpf2203 (AMD EPYC 7763) with evio-interactive-view's `.venv` (pyarrow 21.0.0, numpy 2.0.2). Parquet test files go to node-local `/tmp` and are removed.
- **Ratio** = raw EVIO bytes / compressed bytes. **Speed** = raw EVIO bytes per second on one thread.
- **Hit word** as in evio-interactive-view: `t = bits[30:17]` (4 ns ticks in the 65,536 ns frame), `ch = bits[16:13]`, `q = bits[12:0]`.

## What it showed

| Method | Ratio | Speed (1 thread) |
|---|---|---|
| lz4, raw EVIO | **1.00x** | 2,521 MB/s |
| zstd level 1, raw EVIO | 1.17x | 758 MB/s |
| zstd level 3, raw EVIO | 1.19x | 238 MB/s |
| zstd level 9, raw EVIO | 1.30x | 66 MB/s |
| Each frame compressed on its own (5,000 frames): lz4 / zstd 1 / zstd 3 | 1.00x / 1.16x / 1.16x | 2,002 / 459 / 395 MB/s |
| Hit words only (headers dropped), zstd 3 | 1.20x | |
| Parquet + zstd, hits sorted by (frame, t) | 1.22x | 68 MB/s |
| Parquet + zstd, hits in stream order | 1.41x | 75 MB/s |
| Parquet + zstd, stream order, `t_ns` delta-encoded (`DELTA_BINARY_PACKED`) | 1.65x | 74 MB/s |
| **Hit-aware columns** (below), zstd 1 | **2.32x** | **478 MB/s** |
| Hit-aware columns, zstd 3 | 2.32x | 296 MB/s |

The Parquet columns are `frame, port, ch, t_ns, q`, as `evio_decode.py` writes them (without `roc`, which is constant here). The hit-aware columns are, for each hit: the step in `t` from the previous hit in the same payload bank (the bank's first hit keeps its `t`), `ch` and `q`; plus the number of hits per bank. Each column is byte-shuffled, then compressed with zstd. Frame numbers and port tags follow from the bank order and the counts. The fixed 66 words of headers per frame aren't included: they are 2.8% of the bytes.

### Where the information is

| Field | Bits in the word | Entropy, bits per hit |
|---|---|---|
| `t` (time in frame) | 14 | 13.99: spread evenly over the frame |
| step in `t` within a payload bank | – | 6.75 (1.7% of steps go back in time) |
| `ch` | 4 | 2.51 |
| `q` (charge) | 13 | 5.76 |
| step in `t` + `ch` + `q` | 31 used of 32 | ~15.0 |

Hits come in time order within a payload bank, so the step to the next hit carries about 7 bits where the absolute time needs 14. Adding up the fields gives ~15 bits per 32-bit word, about 2.1x. The measured 2.32x is a little higher because zstd also finds patterns between consecutive values. Without dropping data, 2 to 2.5x is the range to expect.

## Ideas that follow

**Parquet is for analysis, not transfer.** Once headers are only 3% of the bytes, Parquet's gain is the encoding of the hits, and a Parquet row (frame, port, ch, t, q) holds more bytes than the packed 32-bit word it came from. Sorting hits by (frame, t) across ports, as `evio_decode.py` does, mixes the ports and hides the in-bank time order, so it compresses worst (1.22x). Parquet is still the right format for analysis after the data lands: column pruning, frame ranges, and loading into pandas, polars or PyTorch.

**EVIO's own record compression doesn't help.** EVIO records can be LZ4 or gzip compressed. lz4 leaves these frames at 1.00x, and gzip is slower than zstd for a similar ratio. evio-interactive-view and `evio_decode.py` also skip compressed records.

**Compression doesn't fix the losses seen so far.** Runs 1 and 2 used ~1.2 Gbps of the link. Their missing events came in bursts before the receivers, at :27:50 and :57:50 UTC, with zero NIC and socket drops on both nodes ([run 1](../results/2026-11-05-194730UTC-lb677-run1-evio.md#bursts-recur-at-2750-and-5750), [run 2](../results/2026-11-05-201815UTC-lb677-run2-evio.md#the-three-bursts)). Bandwidth wasn't the cause.

**It helps when the full SRO fills the WAN link.** These frames carry one crate. If each of 18 crates sends at this rate, the stream is about 18 x 1.16 ≈ 21 Gbps. A 2.3x hit-aware encoding brings that to ~9 Gbps, and fewer bytes per frame means fewer packets per frame (~7 now, ~3 after), so one dropped packet loses less. The cost at ~480 MB/s per core is about 6 cores for 21 Gbps, at the sender.

**Where it would go:** in the frame builder at the sender, before E2SAR. The LB and `e2sar_daos_recv` don't look inside events, so neither changes. The reader side (`e2sar_daos_read` or the viewer) would decode the frames back to hit words, or write Parquet directly.

**Reducing hits is likely worth more.** 2,360 hits per 65.5 µs frame on 208 channels is ~11 hits per channel per frame, ~170 kHz per channel, with times spread evenly over the frame. That looks like a low threshold or noise rather than physics. A higher threshold, if the sender side can accept one, could cut the stream by more than any lossless encoding.

## Next steps

- Ask the sender side about the threshold and the hit rate per channel, and the expected rate per crate in the 18-crate SRO.
- Repeat on an 18-crate run: hit statistics, and so the ratios, may differ per crate.
- If the WAN link becomes the limit, prototype the hit-aware encoding in the frame builder and check its speed in C++ (this Python measurement uses numpy and pyarrow's zstd).
- Consider having `evio_decode.py` keep hits in stream order (by frame, port, t), which compresses better than sorting by (frame, t).
