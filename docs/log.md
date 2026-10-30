# Directory Update Log

## 2026-10-30
* **Initialization**: Organized the docs as an OKF v0.2 bundle ([#26](https://github.com/JeffersonLab/e2sar-daos/issues/26)): `concepts/`, `howto/`, `sites/`, `reference/` and `results/`, split from DAOS_LAYOUT, DAOS_READER, STREAMING_WORKFLOW, E2E_BENCHMARK, BOTTLENECK_ANALYSIS, ESNET_LB_TESTING, LCF2JLAB_GUIDE, TESTING, AURORA_WORKFLOW, E2SAR_INSTALL and the root, `bin/` and `scripts/` READMEs at 0d85d00. Text moved as it was; each file's `sources` names its origin.
* **Creation**: [Meson options](reference/meson-options.md).
* **Update**: Brought the procedures up to layout v2 ([#31](https://github.com/JeffersonLab/e2sar-daos/pull/31)), one container per run: [lb-test](howto/lb-test.md), [perlmutter](sites/perlmutter.md), [ebpf-testbed](sites/ebpf-testbed.md) and `containers/slurm-tb/README.md` no longer use `--prefix`, `--prefixes` or `--oid`. The unit suite counts are now `Ok: 5`. Results measured on the old KV layout carry `daos_layout: v1`.
* **Update**: One E2SAR environment, in [install-e2sar](howto/install-e2sar.md#the-e2sar-environment-bashrc); the site guides and [lb-test](howto/lb-test.md) link to it and to [build](howto/build.md) instead of repeating them.
* **Creation**: `AGENTS.md` (and `CLAUDE.md`, which imports it) at the repo root points agents at this index.
* **Update**: Fixed four links whose anchors no longer existed.
