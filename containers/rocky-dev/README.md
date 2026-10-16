
## The container for e2sar-daos development

A Rocky Linux 9 image with everything needed to build and unit-test e2sar-daos:

| Component | Source | Location in image |
|---|---|---|
| E2SAR (+ bundled gRPC, protobuf, Boost) | [E2SAR release RPM](https://github.com/JeffersonLab/E2SAR/releases) | `/usr/local` |
| DAOS client + headers (`daos-client`, `daos-devel`) | [packages.daos.io](https://packages.daos.io) v2.6 / EL9 | `/usr/include`, `/usr/lib64` |
| gcc/g++, meson, ninja, gdb | Rocky BaseOS / CRB / EPEL | system |

**The e2sar-daos source is not in the image.** Mount the repo into the container at `/work`.

There is **no DAOS server** in the image, and `daos_agent` is not running. Anything that needs to
connect to a real pool still has to run on a testbed. Compiling, linking and the header/version unit
tests all work.

Because DAOS and E2SAR only ship `x86_64` packages, the image is always `linux/amd64`. On Apple
Silicon Macs it runs under emulation, so expect it to be slower than native.

---

### Build arguments

| Arg | Default | Notes |
|---|---|---|
| `ROCKY_VERSION` | `9.6` | Must match an E2SAR release, e.g. `9.0`, `9.5` or `9.6` |
| `E2SAR_VERSION` | `0.4.0rc1` | From the release tag `E2SAR-<ver>-main-rocky-<rocky_ver>` |
| `DAOS_VERSION` | `2.6` | Must have an `EL9` repo on packages.daos.io |

### Networks that intercept TLS (JLab)

The JLab border firewall re-signs HTTPS traffic with the JLab CA. Without that CA, `dnf` and `curl`
inside the build fail with `SSL certificate problem: self-signed certificate in certificate chain`.
Any `*.crt` or `*.pem` file in [`certs/`](./certs/) is trusted during the build. These files are
git-ignored.

On a Mac, export the JLab CAs from the keychain:

```bash
cd containers/rocky-dev
for cn in JLabCA JLABICA8-JLabGPCA; do
    security find-certificate -c "$cn" -p /Library/Keychains/System.keychain > certs/$cn.crt
done
```

Off-site, leave `certs/` empty.

---

### Testing on a Mac (Apple Silicon or Intel)

Prerequisite: a Docker engine such as Docker Desktop, Rancher Desktop (`moby` engine) or colima,
with amd64 emulation turned on. Rosetta is the faster option. Check it with:

```bash
docker run --rm --platform linux/amd64 rockylinux/rockylinux:9.6 uname -m   # -> x86_64
```

1. Build the image. This takes a few minutes, mostly downloading the ~165 MB E2SAR RPM.

    ```bash
    cd containers/rocky-dev
    docker build --platform linux/amd64 -t e2sar-daos:dev .
    ```

2. Build and test e2sar-daos inside the container. Run this from the repo root:

    ```bash
    # One-shot: configure, compile, run the unit tests
    docker run --rm --platform linux/amd64 -v "$PWD":/work e2sar-daos:dev \
        bash -c "meson setup --wipe build && meson compile -C build && meson test -C build --suite unit -v"

    # Or open an interactive shell and run the same commands yourself
    docker run -it --rm --platform linux/amd64 -v "$PWD":/work e2sar-daos:dev
    ```

    The `build/` directory is created in your checkout, so later runs can skip `--wipe`.
    It holds Linux binaries and cannot run on macOS.

### Using podman (JLab testbeds)

The same commands work with `podman` in place of `docker`. On an x86_64 Linux host, `--platform` is
not needed. Add `--network host` if the code has to reach DAOS services on the host network.

```bash
podman build -t e2sar-daos:dev containers/rocky-dev
podman run -it --rm --network host -v "$PWD":/work e2sar-daos:dev
```
