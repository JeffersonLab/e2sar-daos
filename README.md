# The DAOS interface for E2SAR
A C++ library and applications for storing E2SAR data streams in [DAOS](https://docs.daos.io).

- `libe2sar-daos` ([include/e2sar_daos/](include/e2sar_daos/)) provides RAII handles for the DAOS runtime, pool, container (created if missing) and KV object, the run catalog (`RunCatalog`: one object per run, ids derived from the run's prefix), plus `AsyncKVWriter`, which issues non-blocking `daos_kv_put`s on a DAOS event queue with a bounded number in flight.
- [Applications](bin/README.md):
    - `e2sar_daos_recv` receives events from an E2SAR sender and writes them to DAOS: small events batched into 1 MiB chunks, large ones written directly, asynchronously.
    - `e2sar_daos_read` reads a run back while it is written (`--live`, `--live-rm`) or one file per event (`--offline`), and lists, verifies and deletes runs.
    - One run is one container: byte arrays per UTC second and writer, an index per second, and a catalog. See [the DAOS layout](docs/concepts/daos-layout.md) and [the reader](docs/reference/e2sar_daos_read.md).
- [Scripts](docs/reference/scripts.md) that drive the applications on the JLab test nodes: the end-to-end bench, its rate sweep, and the LCF receiver.

- Refer to [containers](containers/rocky-dev/) for the Rocky 9 + DAOS 2.6 development environment. It runs on JLab testbeds and on Macs.
- Unit testing is turned on by default in [meson_options.txt](meson_options.txt).
- `meson install -C build` installs the library, headers and the two applications under the configured prefix (default `/usr/local`).

## Build

```bash
meson setup build            # add -Dbuildtest=false to skip tests
meson compile -C build
meson test -C build
```

This needs a C++17 compiler, the DAOS client and headers, Meson and Ninja, and an installed [E2SAR](https://github.com/JeffersonLab/E2SAR) that pkg-config can find, with `BOOST_ROOT` pointing at E2SAR's Boost. See [docs/howto/build.md](docs/howto/build.md) for the details, the container build and the integration test, and [docs/howto/install-e2sar.md](docs/howto/install-e2sar.md) to install E2SAR.

## Documentation

Start at [docs/index.md](docs/index.md). The docs are an [Open Knowledge Format (OKF)](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md) bundle: one topic per file, each with a short YAML header (`type`, `description`, and `stale_after` where a procedure may be out of date).

| Folder | What's in it |
|---|---|
| [concepts/](docs/concepts/index.md) | The DAOS layout, the streaming workflow, the throughput metrics, the lossless ceiling |
| [howto/](docs/howto/index.md) | Build, install E2SAR, run the tests, the end-to-end checks, the LB test, the bench |
| [sites/](docs/sites/index.md) | Mac, the ebpf testbed, the Slurm TB, Perlmutter, Aurora |
| [reference/](docs/reference/index.md) | `e2sar_daos_recv`, `e2sar_daos_read`, `lbadm`, the scripts, the Meson options |
| [results/](docs/results/index.md) | Dated measurements |
