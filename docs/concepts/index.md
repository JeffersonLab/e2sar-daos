# Concepts

How e2sar-daos stores and moves data, and how its numbers are defined.

* [Data layout in DAOS](daos-layout.md) - Layout v2: one container per run, with a catalog, a seconds object, an index per UTC second and byte arrays per second and writer; the writer–reader contract.
* [Streaming workflow: senders → LB → DAOS → live reader](streaming-workflow.md) - How senders, the EJFAT LB, the receivers (DAOS writers) and a live reader work together, what the reader guarantees, the two live monitors, and what streaming senders change.
* [Throughput and latency metrics](throughput-metrics.md) - Definitions of sender goodput, ingest, DAOS commit, received and E2E rates, the per-stage latencies, and the loss counters used in every result.
* [The lossless ceiling and the port discards](lossless-ceiling.md) - Why runs are lossless only up to ~25 Gbps node-to-node and 25–35 Gbps through the LB: NIC port discards on ebpf2203, the possible causes, and the runs that would tell them apart.
