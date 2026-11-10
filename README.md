# The DAOS interface for E2SAR
A C++ library and applications for storing E2SAR data streams in [DAOS](https://docs.daos.io).

- `libe2sar-daos` ([include/e2sar_daos/](include/e2sar_daos/)) writes and reads runs in DAOS. One run is one container: byte arrays per UTC second and writer, an index per second, and a catalog. What each holds, and the contract between writers and readers: [the DAOS layout](docs/concepts/daos-layout.md).
- [Applications](bin/README.md):
    - `e2sar_daos_recv` receives events from an E2SAR sender and writes them to DAOS: small events batched into 1 MiB chunks, large ones written directly, asynchronously.
    - `e2sar_daos_read` reads a run back while it is written (`--live`, `--live-rm`) or one file per event (`--offline`), and lists, verifies and deletes runs. See [the reader](docs/reference/e2sar_daos_read.md).
    - `e2sar_daos_monitor` watches a run's container, read-only, and reports the events coming in, leaving and missing every 2 s, for the dashboard below.
- [Scripts](docs/reference/scripts.md) that drive the applications on the JLab test nodes: the end-to-end bench, its rate sweep, the receiver tuning sweep, and the LCF receiver.

- Refer to [containers](containers/rocky-dev/) for the Rocky 9 + DAOS 2.6 development environment. It runs on JLab testbeds and on Macs.
- Unit testing is turned on by default in [meson_options.txt](meson_options.txt).
- `meson install -C build` installs the library, headers and the applications under the configured prefix (default `/usr/local`).

## Live monitoring

A run can be watched live in a browser, for example on a laptop that reaches the reader node only through SSH:

- **DAOS monitor:** [`e2sar_daos_monitor`](docs/reference/e2sar_daos_monitor.md) writes the container's state as JSON lines, and [`viz/daos_monitor/`](viz/daos_monitor/) serves them as a dashboard: events per second in and out of DAOS, data waiting in DAOS, each writer's lag, and every `dataId`'s event numbers, with missing ones marked. **Replay** plays a finished run back from its JSON lines file, without DAOS.
- **EVIO live viewer:** [evio-interactive-view](https://code.jlab.org/xmei/evio-interactive-view) (`serve.py --live`) decodes the newest file the reader wrote: hits per channel × time in frame, per crate.

Both only read, so the receivers and the reader run the same with or without them. The steps for the SC26 booth, including a demo mode without DAOS, are in [docs/howto/live-monitoring.md](docs/howto/live-monitoring.md).

![The DAOS monitor on 2026-11-10: frames_thread0_file0020.evio sent through LB 682 at 15 Gbps (250 frames, ~100 KB, per event; 4 streams), ramped up from 1 Gbps over the first 10 s (e2sar_evio_send --ramp 10): 1,657,756 events sent, all stored, 0 missing; without the ramp 15 Gbps lost 21. Read with e2sar_daos_read --live-rm --interval 2 on the receiver node; the reader writes ~2 Gbps, so data waits in DAOS (In DAOS grows) while Outgoing counts seconds of data removed](docs/images/sc26_daos_monitor.png)

## Build

```bash
meson setup build            # add -Dbuildtest=false to skip tests
meson compile -C build
meson test -C build --suite unit   # the integration suite needs a DAOS pool
```

This needs a C++17 compiler, the DAOS client and headers, Meson and Ninja, and an installed [E2SAR](https://github.com/JeffersonLab/E2SAR) that pkg-config can find, with `BOOST_ROOT` pointing at E2SAR's Boost. See [docs/howto/build.md](docs/howto/build.md) for the details, the container build and the integration test, and [docs/howto/install-e2sar.md](docs/howto/install-e2sar.md) to install E2SAR.

## Documentation

Start at [docs/index.md](docs/index.md). The docs are an [Open Knowledge Format (OKF)](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md) bundle: one topic per file, each with a short YAML header (`type`, `description`, and `stale_after` where a procedure may be out of date).

| Folder | What's in it |
|---|---|
| [concepts/](docs/concepts/index.md) | The DAOS layout, the streaming workflow, the throughput metrics, the lossless ceiling |
| [howto/](docs/howto/index.md) | Build, install E2SAR, run the tests, the end-to-end checks, the streaming walkthrough, live monitoring, the LB test, the bench |
| [sites/](docs/sites/index.md) | JLab ebpf testbed, JLab Slurm TB, Perlmutter (NERSC), Aurora (ALCF), LUX (OLCF) |
| [reference/](docs/reference/index.md) | `e2sar_daos_recv`, `e2sar_daos_read`, `e2sar_daos_monitor`, `lbadm`, the scripts, the Meson options |
| [results/](docs/results/index.md) | Dated measurements |
