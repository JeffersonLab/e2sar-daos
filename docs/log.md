# Directory Update Log

## 2026-10-30
* **Initialization**: Organized the docs as an OKF v0.2 bundle ([#26](https://github.com/JeffersonLab/e2sar-daos/issues/26)): `concepts/`, `howto/`, `sites/`, `reference/` and `results/`, split from DAOS_LAYOUT, DAOS_READER, STREAMING_WORKFLOW, E2E_BENCHMARK, BOTTLENECK_ANALYSIS, ESNET_LB_TESTING, LCF2JLAB_GUIDE, TESTING, AURORA_WORKFLOW, E2SAR_INSTALL and the root, `bin/` and `scripts/` READMEs at 0d85d00. Text moved as it was; each file's `sources` names its origin.
* **Creation**: [Meson options](reference/meson-options.md).
* **Update**: Marked stale since layout v2 ([#31](https://github.com/JeffersonLab/e2sar-daos/pull/31)), with `stale_after: 2026-10-29T23:15:16Z`: [lb-test](howto/lb-test.md), [perlmutter](sites/perlmutter.md) and [ebpf-testbed](sites/ebpf-testbed.md) still use `--prefix`. Results measured on the old KV layout carry `daos_layout: v1`.
* **Update**: Fixed four links whose anchors no longer existed.
