---
type: How-To
title: "Build e2sar-daos"
description: "Prerequisites, pointing pkg-config and Meson at E2SAR and its Boost, configure/compile/test, rebuilds, the container build and the integration test."
tags: [build, meson, e2sar, boost]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/README.md", title: "README.md at 0d85d00" }
---

# Build

## Prerequisites
- A C++17 compiler (e.g. GCC 11).
- DAOS client and headers: `sudo dnf install daos-client daos-devel` (installs `/usr/include/daos*.h` and `/usr/lib64/libdaos.so`, `libdfs.so`).
- Boost `program_options`, `log`, `log_setup`, `thread`, `filesystem`, `chrono`, `url`, plus `unit_test_framework` for tests (`-Dbuildtest=false` to skip). Use the Boost bundled with E2SAR, not the system one (see below).
- An installed [E2SAR](https://github.com/JeffersonLab/E2SAR) with its `e2sar.pc`. To install the latest GitHub release, with or without root, see [install-e2sar](install-e2sar.md).
- Meson and Ninja. Without root, install into `~/.local/bin`:
    ```bash
    python3 -m pip install --user meson ninja
    export PATH="$HOME/.local/bin:$PATH"   # add to ~/.bashrc
    ```

## Point pkg-config and Meson to E2SAR
If E2SAR was installed with a staging root (e.g. `DESTDIR=~/opt/e2sar`, so the `*.pc` files still say `prefix=/usr/local`), set both the search path and the sysroot to the staging root itself, not to `$E2SAR_ROOT/usr/local`:
```bash
E2SAR_ROOT=$HOME/opt/e2sar
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
pkg-config --modversion e2sar   # sanity check
pkg-config --libs e2sar         # confirm it resolves to $E2SAR_ROOT/usr/local/lib* and not /usr/lib64
```
If E2SAR was installed directly to its prefix, only `PKG_CONFIG_PATH` is needed.

`libe2sar` is linked against the Boost installed with E2SAR. Meson's Boost lookup does not use pkg-config, so without `BOOST_ROOT` it picks up the system Boost (e.g. `/usr/include/boost`), which may be a different version or ABI. Set `BOOST_ROOT` to the E2SAR install prefix before `meson setup`:
```bash
export BOOST_ROOT=$E2SAR_ROOT/usr/local    # staging root, as above
# export BOOST_ROOT=/usr/local             # E2SAR installed directly to its prefix (e.g. the RPM)
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH   # to run the binaries and tests
```
`meson setup` prints `Boost version: ...` in the test section; it should match E2SAR's Boost. After changing `BOOST_ROOT`, reconfigure with `meson setup --wipe build`.

If the link fails with undefined references such as `grpc::experimental::NoOpCertificateVerifier` or `grpc::experimental::TlsCredentialsOptions`, the wrong E2SAR gRPC ABI is being selected. In that case, make sure `pkg-config --libs e2sar` shows the E2SAR install under `$E2SAR_ROOT`, not a system `/usr/lib64` install, and rebuild from a fresh `meson setup --wipe build`.

## Configure, compile, and test
```bash
meson setup build            # add -Dbuildtest=false to skip tests
meson compile -C build
meson test -C build
```

## Rebuild and options
```bash
# Incremental rebuild after editing sources or meson.build
meson compile -C build

# Toggle tests on an existing build dir
meson configure build -Dbuildtest=false
meson configure build -Dbuildtest=true

# Reconfigure from scratch (e.g. after changing PKG_CONFIG_PATH, BOOST_ROOT or dependencies)
meson setup --wipe build

# Show current build options
meson configure build | grep -A1 buildtest

# Run tests verbosely, or only a suite
meson test -C build -v
meson test -C build --suite daos
```

## Or build in a container (Mac or Linux)

```bash
# From the repo root. On JLab networks, first add the JLab CA certs to
# containers/rocky-dev/certs/ (see containers/rocky-dev/README.md).
docker build --platform linux/amd64 -t e2sar-daos:dev containers/rocky-dev

docker run --rm --platform linux/amd64 -v "$PWD":/work e2sar-daos:dev \
    bash -c "meson setup build && meson compile -C build && meson test -C build --suite unit -v"
```

On a Mac the container runs as `linux/amd64` under emulation, because DAOS and E2SAR only ship x86_64 packages.
There is no DAOS server in the container, so it covers compiling, linking and unit tests only.

## Integration test against a live DAOS system

See [run-tests](run-tests.md) for testing on a Mac, on ebpf2201/ebpf2203, and on Slurm TB nodes.

`DAOSKVTest` (suite `integration`) writes 5 random 0.5 MiB values into a DAOS KV object, reads them back and checks the SHA-256 of each. It needs `openssl-devel` and access to a DAOS system (e.g. the JLab testbeds), with the pool and container already created:
```bash
daos cont create sc26 kv-test          # once; the pool is created by an admin
```
The pool and container default to `sc26` and `kv-test`. `meson test` passes them to the test as `DAOS_POOL` and `DAOS_CONT`:
```bash
meson test -C build --suite integration -v
meson configure build -Ddaos_pool=mypool -Ddaos_cont=mycont   # use another pool/container
DAOS_POOL=mypool DAOS_CONT=mycont ./build/tests/daos/daos_kv_test --log_level=message   # run directly
```
