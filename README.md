# The DAOS interface for E2SAR
A C++ wrapper for DAOS client header files to fit into the E2SAR data streaming use cases.

- Refer to [containers](./containers/rocky-dev/) for the Rocky 9 + DAOS 2.6 development environment. It runs on JLab testbeds and on Macs.
- Unit testing is turned on by default in [meson_options.txt](./meson_options.txt).
- Installing library files are turned off at the moment in the top-level [meson.build](./meson.build).

## Build

### Prerequisites
- A C++17 compiler (e.g. GCC 11).
- DAOS client and headers: `sudo dnf install daos-client daos-devel` (installs `/usr/include/daos*.h` and `/usr/lib64/libdaos.so`, `libdfs.so`).
- Boost `unit_test_framework` (only for tests; `-Dbuildtest=false` to skip).
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

# Reconfigure from scratch (e.g. after changing PKG_CONFIG_PATH or dependencies)
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
