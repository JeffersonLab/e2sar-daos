---
okf_version: 0.2
---
# e2sar-daos documentation

The docs for [e2sar-daos](../README.md), as an [Open Knowledge Format (OKF) v0.2](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md) bundle. Each file covers one topic and starts with a YAML header: `type`, `title`, `description`, and where it applies `status` (`draft`: not run yet), `stale_after` (check the file before use once this date has passed) and `daos_layout` (results: which DAOS layout the numbers are for). Changes to the bundle are in [log.md](log.md).

# Folders

* [Concepts](concepts/index.md) - The DAOS layout, the streaming workflow, the throughput and latency metrics, the lossless ceiling
* [How-to guides](howto/index.md) - Build, install E2SAR, run the tests, the end-to-end checks, the LB test, the bench
* [Site guides](sites/index.md) - Mac, the ebpf testbed, the Slurm TB, Perlmutter, Aurora
* [Reference](reference/index.md) - `e2sar_daos_recv`, `e2sar_daos_read`, `lbadm`, the scripts, the Meson options
* [Results](results/index.md) - Dated measurements, newest first

# Start here

* [Build e2sar-daos](howto/build.md) - Prerequisites, pointing Meson at E2SAR and its Boost, configure, compile, test
* [Data layout in DAOS](concepts/daos-layout.md) - What a run looks like in DAOS, and the writer–reader contract
* [Streaming workflow](concepts/streaming-workflow.md) - Senders → LB → DAOS → live reader
* [Testing e2sar-daos](howto/run-tests.md) - Which test runs where
* [End-to-end results: summary](results/e2e-summary.md) - The highest lossless rates and what limits them
