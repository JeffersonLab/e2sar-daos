# Concepts

How e2sar-daos stores and moves data, and how its numbers are defined.

* [Data layout in DAOS](daos-layout.md) - One container per run, with a catalog, a seconds object, an index per UTC second and byte arrays per second and writer; the writer–reader contract.
* [Streaming workflow: senders → LB → DAOS → live reader](streaming-workflow.md) - How senders, the EJFAT LB, the receivers (DAOS writers) and a live reader work together, what the reader guarantees, the two live monitors, and what streaming senders change.
* [Throughput and latency metrics](throughput-metrics.md) - Definitions of sender goodput, ingest, DAOS commit, received and E2E rates, the per-stage latencies, and the loss counters used in every result.
* [The lossless ceiling and the port discards](lossless-ceiling.md) - Why runs are lossless only up to ~25 Gbps node-to-node and 25–35 Gbps through the LB: NIC port discards on ebpf2203, the possible causes, and the runs that would tell them apart.
* [Compressing hit frames for the WAN: ideas and a first look](hit-compression.md) - Prototyping ideas, not a result: how much the EVIO time frames of LB 677 run 1 (97% fADC hit words) shrink, measured on 20,000 frames: lz4 1.00x, zstd 1.17-1.30x, Parquet 1.22-1.65x, and 2.32x for a hit-aware encoding (time step within a port bank, channel, charge) at ~480 MB/s per core. With so few header words Parquet gains little in size, unlike the cosmics file. Compression doesn't touch the losses seen so far (periodic bursts, not bandwidth), but would cut the WAN rate of the full 18-crate SRO by about 2.3x.
