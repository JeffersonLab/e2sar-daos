# check=skip=FromPlatformFlagConstDisallowed
# A Rocky 9 image with all e2sar-daos build dependencies: E2SAR (prebuilt RPM,
# which bundles gRPC/protobuf/Boost) and the DAOS client + headers.
#
# DAOS and E2SAR only publish x86_64 packages, so the image is pinned to
# linux/amd64. On Apple Silicon Macs it runs under emulation (Rosetta/QEMU).

# Parameters can be configured with "--build-arg"
ARG ROCKY_VERSION=9.6

FROM --platform=linux/amd64 rockylinux/rockylinux:${ROCKY_VERSION}

# E2SAR release tags look like "E2SAR-<ver>-main-rocky-<rocky_ver>",
# see https://github.com/JeffersonLab/E2SAR/releases.
ARG ROCKY_VERSION=9.6
ARG E2SAR_VERSION=0.4.0rc1
# DAOS version matching the JLab testbeds.
ARG DAOS_VERSION=2.6

## Extra CA certificates: any *.crt/*.pem placed in ./certs is trusted. Needed
## on networks that re-sign TLS traffic, e.g. the JLab border firewall.
COPY certs/ /tmp/certs/
RUN find /tmp/certs \( -name '*.crt' -o -name '*.pem' \) \
        -exec cp {} /etc/pki/ca-trust/source/anchors/ \; && \
    update-ca-trust && \
    rm -rf /tmp/certs

## Basic dev packages (meson/ninja come from CRB/EPEL)
RUN dnf -y install dnf-plugins-core epel-release && \
    dnf config-manager --set-enabled crb && \
    dnf -y install \
        gcc \
        gcc-c++ \
        gdb \
        git \
        make \
        which \
        pkgconf-pkg-config \
        openssl-devel \
        re2-devel \
        zlib-devel \
        meson \
        ninja-build && \
    dnf clean all && \
    rm -rf /var/cache/dnf /var/tmp/*

## E2SAR: installs everything under /usr/local
RUN E2SAR_TAG="E2SAR-${E2SAR_VERSION}-main-rocky-${ROCKY_VERSION}" && \
    curl -fL -o /tmp/e2sar.rpm \
        "https://github.com/JeffersonLab/E2SAR/releases/download/${E2SAR_TAG}/e2sar-${E2SAR_VERSION}-1.x86_64.rpm" && \
    dnf -y install /tmp/e2sar.rpm && \
    rm -f /tmp/e2sar.rpm && \
    dnf clean all && \
    rm -rf /var/cache/dnf /var/tmp/*

ENV PKG_CONFIG_PATH="/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig" \
    LD_LIBRARY_PATH="/usr/local/lib64:/usr/local/lib" \
    BOOST_ROOT="/usr/local"

## DAOS client libraries and headers (libdaos, libdfs, <daos.h>)
# NOTE: there is no DAOS server in this image; daos_agent is installed but not
# started, so only code that does not connect to a pool can run here.
RUN curl -fL -o /etc/yum.repos.d/daos-packages.repo \
        "https://packages.daos.io/v${DAOS_VERSION}/EL9/packages/x86_64/daos_packages.repo" && \
    dnf -y install daos-client daos-devel && \
    dnf clean all && \
    rm -rf /var/cache/dnf /var/tmp/*

WORKDIR /work
CMD ["/bin/bash"]
