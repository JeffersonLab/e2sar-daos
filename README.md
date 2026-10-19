# The DAOS interface for E2SAR
A C++ library and applications for storing E2SAR data streams in [DAOS](https://docs.daos.io).

- `libe2sar-daos` ([include/e2sar_daos/](./include/e2sar_daos/)) provides RAII handles for the DAOS runtime, pool, container (created if missing) and KV object, plus `AsyncKVWriter`, which issues non-blocking `daos_kv_put`s on a DAOS event queue with a bounded number in flight.
- [Applications](./bin/README.md):
    - `e2sar_daos_recv` receives events from `e2sar_ft` on another node and writes them to DAOS.
    - `e2sar_daos_read` lists the stored events, verifies them and reads them back.

- Refer to [containers](./containers/rocky-dev/) for the Rocky 9 + DAOS 2.6 development environment. It runs on JLab testbeds and on Macs.
- Unit testing is turned on by default in [meson_options.txt](./meson_options.txt).
- `meson install -C build` installs the library, headers and the two applications under the configured prefix (default `/usr/local`).

## Build

### Prerequisites
- A C++17 compiler (e.g. GCC 11).
- DAOS client and headers: `sudo dnf install daos-client daos-devel` (installs `/usr/include/daos*.h` and `/usr/lib64/libdaos.so`, `libdfs.so`).
- Boost `program_options`, `log`, `log_setup`, `thread`, `filesystem`, `chrono`, `url`, plus `unit_test_framework` for tests (`-Dbuildtest=false` to skip). Use the Boost bundled with E2SAR, not the system one (see below).
- An installed [E2SAR](https://github.com/JeffersonLab/E2SAR) with its `e2sar.pc`.
- Meson and Ninja. Without root, install into `~/.local/bin`:
    ```bash
    python3 -m pip install --user meson ninja
    export PATH="$HOME/.local/bin:$PATH"   # add to ~/.bashrc
    ```

### Point pkg-config to E2SAR
If E2SAR was installed with a staging root (e.g. `DESTDIR=~/opt/e2sar`, so the `*.pc` files still say `prefix=/usr/local`), set both the search path and the sysroot:
```bash
E2SAR_ROOT=$HOME/opt/e2sar
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
pkg-config --modversion e2sar   # sanity check
```
If E2SAR was installed directly to its prefix, only `PKG_CONFIG_PATH` is needed.

### Point Meson to E2SAR's Boost
`libe2sar` is linked against the Boost installed with E2SAR. Meson's Boost lookup does not use pkg-config, so without `BOOST_ROOT` it picks up the system Boost (e.g. `/usr/include/boost`), which may be a different version or ABI. Set `BOOST_ROOT` to the E2SAR install prefix before `meson setup`:
```bash
export BOOST_ROOT=$E2SAR_ROOT/usr/local    # staging root, as above
# export BOOST_ROOT=/usr/local             # E2SAR installed directly to its prefix (e.g. the RPM)
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH   # to run the binaries and tests
```
`meson setup` prints `Boost version: ...` in the test section; it should match E2SAR's Boost. After changing `BOOST_ROOT`, reconfigure with `meson setup --wipe build`.

### Configure, compile, and test
```bash
meson setup build            # add -Dbuildtest=false to skip tests
meson compile -C build
meson test -C build
```

### Rebuild and options
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

### Or build in a container (Mac or Linux)

```bash
# From the repo root. On JLab networks, first add the JLab CA certs to
# containers/rocky-dev/certs/ (see containers/rocky-dev/README.md).
docker build --platform linux/amd64 -t e2sar-daos:dev containers/rocky-dev

docker run --rm --platform linux/amd64 -v "$PWD":/work e2sar-daos:dev \
    bash -c "meson setup build && meson compile -C build && meson test -C build --suite unit -v"
```

On a Mac the container runs as `linux/amd64` under emulation, because DAOS and E2SAR only ship x86_64 packages.
There is no DAOS server in the container, so it covers compiling, linking and unit tests only.

### Integration test against a live DAOS system

See [docs/TESTING.md](docs/TESTING.md) for testing on a Mac, on ebpf2201/ebpf2203, and on Slurm TB nodes.


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
