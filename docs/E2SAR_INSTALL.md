# Installing the latest E2SAR from a GitHub release

e2sar-daos builds against a prebuilt [E2SAR](https://github.com/JeffersonLab/E2SAR), and uses its tools (`e2sar_ft`, `lbadm`) for testing.
This page shows how to install the latest release from [JeffersonLab/E2SAR releases](https://github.com/JeffersonLab/E2SAR/releases), with root (section A) or without it (section B), and then how to point e2sar-daos at it.
Last checked on 2026-10-19 on ebpf2201 (AlmaLinux 9.8): the latest release was `E2SAR-0.4.0rc1-main-rocky-9.6`. Section B was run end to end, and e2sar-daos built against the result with `unit` at Ok: 3, Fail: 0.

## 1. Pick the release and package

Each E2SAR version is published as one release per distro, with tags of the form `E2SAR-<version>-<branch>-<distro>`:

| Tag suffix | Assets | Use on |
|---|---|---|
| `rocky-9.6` (marked *Latest*) | `e2sar-<ver>-1.x86_64.rpm`, `e2sar-deps-<ver>-1.x86_64.rpm` | Rocky / AlmaLinux / RHEL 9.6 and later (ebpf220x, Slurm TB, the rocky-dev container) |
| `rocky-9.5`, `rocky-9.0` | same | Older EL9 minor releases |
| `ubuntu-22.04`, `debian-12` | `e2sar_<ver>_amd64.deb`, `e2sar-deps_<ver>_amd64.deb` | Ubuntu / Debian |

All packages are x86_64 only.

**Install only `e2sar`.** The `e2sar` package already contains everything in `e2sar-deps` (gRPC, protobuf, abseil, Boost: 18,104 shared files), plus the E2SAR library, headers and tools.
`e2sar-deps` exists for building E2SAR yourself. Installing both makes the files collide.
Everything installs under `/usr/local`, and the extracted tree takes about 800 MB.

Find the latest tag and version:

```bash
TAG=$(curl -fsSL https://api.github.com/repos/JeffersonLab/E2SAR/releases/latest | grep -m1 '"tag_name"' | cut -d'"' -f4)
VER=$(echo $TAG | sed -E 's/^E2SAR-([^-]+)-.*/\1/')
echo $TAG $VER                    # E2SAR-0.4.0rc1-main-rocky-9.6 0.4.0rc1
# or, with the GitHub CLI:  gh release list -R JeffersonLab/E2SAR -L 5
```
`releases/latest` returns the EL9 build that's marked *Latest*. For another distro, take the tag from `gh release list`, then set `TAG` and `VER` by hand.

Download (over HTTPS; GitHub over SSH is blocked from the testbed nodes):

```bash
mkdir -p ~/pkg && cd ~/pkg
curl -fLO https://github.com/JeffersonLab/E2SAR/releases/download/$TAG/e2sar-$VER-1.x86_64.rpm
rpm -qpi e2sar-$VER-1.x86_64.rpm | grep -E '^(Name|Version)'      # e2sar, 0.4.0rc1
```

## A. With root: install the package

```bash
sudo dnf install ./e2sar-$VER-1.x86_64.rpm          # or: sudo dnf upgrade ./e2sar-$VER-1.x86_64.rpm
rpm -q e2sar
```

On Ubuntu or Debian, download `e2sar_${VER}_amd64.deb` from the matching tag instead, and run `sudo apt install ./e2sar_${VER}_amd64.deb` (not tested here).

The package installs into `/usr/local` directly, so you need only:
```bash
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig
export BOOST_ROOT=/usr/local
export LD_LIBRARY_PATH=/usr/local/lib64:/usr/local/lib:$LD_LIBRARY_PATH
```
This is what [containers/rocky-dev/Dockerfile](../containers/rocky-dev/Dockerfile) does. To change the version there, use `--build-arg E2SAR_VERSION=<ver> --build-arg ROCKY_VERSION=<9.x>`.

A system-wide install replaces any older E2SAR in `/usr/local` for every user on the node.
ebpf2203, for example, has the 0.3.1 RPM there. Don't upgrade a shared node without asking its admins. Use section B instead.

## B. Without root: extract into your home (used on the testbed)

The RPM is a plain archive. Unpack it into a directory of your own:

```bash
DEST=$HOME/opt/e2sar-$VER
mkdir -p $DEST && cd $DEST
rpm2cpio ~/pkg/e2sar-$VER-1.x86_64.rpm | cpio -idm --quiet
ls $DEST/usr/local                     # bin include lib lib64 share
ln -sfn $DEST $HOME/opt/e2sar          # optional: a stable path that the env below uses
```
On Debian/Ubuntu, the equivalent is `dpkg-deb -x e2sar_${VER}_amd64.deb $DEST`.

The `*.pc` files inside still say `prefix=/usr/local`, so treat `$DEST` as a staging root. Set these in each shell, or in your env file:

```bash
export E2SAR_ROOT=$HOME/opt/e2sar
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
export LD_LIBRARY_PATH=$BOOST_ROOT/lib64:$BOOST_ROOT/lib:$LD_LIBRARY_PATH
export E2SAR_BIN=$E2SAR_ROOT/usr/local/bin
```

On the ebpf nodes, `$HOME` is shared, so one extraction serves both nodes.
`~/opt/e2sar` there is a plain directory holding 0.4.0rc1, not a symlink. Before you `ln -sfn` over it, move it aside, for example to `~/opt/e2sar-0.4.0rc1.old`.

## 2. Verify

```bash
$E2SAR_BIN/e2sar_ft --help | head -1          # E2SAR Version: 0.4.0rc1      (with root: /usr/local/bin/e2sar_ft)
$E2SAR_BIN/lbadm --help | head -1             # E2SAR Version: 0.4.0rc1
ldd $E2SAR_BIN/e2sar_ft | grep 'not found'    # no output
pkg-config --modversion e2sar                 # 0.4.0rc1
pkg-config --libs e2sar                       # paths under $E2SAR_ROOT/usr/local, not /usr/lib64
```

Always call the tools by full path (`$E2SAR_BIN/e2sar_ft`). If an older E2SAR is also in `/usr/local/bin` (ebpf2203 has 0.3.1), a bare `e2sar_ft` runs that one instead.

If you have an LB instance URI (see [EJFAT_LB_TESTING.md](EJFAT_LB_TESTING.md)), check the control plane too:
```bash
$E2SAR_BIN/lbadm --status                     # LB ID, registered senders and workers
```

## 3. Build e2sar-daos against it

```bash
export PATH=$HOME/.local/bin:$PATH            # meson, ninja
cd ~/daq/e2sar-daos
meson setup --wipe build                      # --wipe after an E2SAR upgrade
meson compile -C build
meson test -C build --suite unit              # Ok: 3, Fail: 0
```
The project version printed by `meson setup` includes the E2SAR version, for example `0.1.0-e2sar_0.4.0rc1`.
For link errors such as `grpc::experimental::...` undefined references, see [README.md, Build](../README.md#point-pkg-config-and-meson-to-e2sar).

## Upgrading

1. Find the new `TAG` and `VER` (step 1), then download and extract into a new `$HOME/opt/e2sar-$VER` (section B).
2. Point the symlink at it: `ln -sfn $HOME/opt/e2sar-$VER $HOME/opt/e2sar`.
3. Rebuild e2sar-daos with `meson setup --wipe build` (step 3), and rerun the unit suite.
4. Keep the old directory until the new one has passed a test run, so you can roll back by moving the symlink back.

Read the release notes before upgrading. Command-line options of `e2sar_ft` and `lbadm` can change between versions, and the test procedures in this directory were written against 0.4.0rc1.
