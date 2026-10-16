# Runtime image for the JLab Slurm TB nodes: the Rocky 9 dev image plus e2sar-daos
# built and installed into /usr/local, so the nodes need nothing but podman and a
# running daos_agent. The E2SAR RPM in the base image also provides e2sar_ft, so the
# same image can be the sender on one node and e2sar_daos_recv on another.
#
# Build from the repo root (the build context must be the whole repo):
#   podman build -f containers/slurm-tb/Dockerfile -t e2sar-daos:tb .
#
# See containers/slurm-tb/README.md for how to run it.

# The base image is containers/rocky-dev, built first as e2sar-daos:dev.
ARG BASE_IMAGE=e2sar-daos:dev
FROM ${BASE_IMAGE}

COPY . /src/e2sar-daos

RUN meson setup /tmp/build /src/e2sar-daos \
        --buildtype=release --prefix=/usr/local -Dbuildtest=false && \
    meson compile -C /tmp/build && \
    meson install -C /tmp/build && \
    ldconfig && \
    rm -rf /tmp/build

# Smoke check at build time: the apps start and resolve all shared libraries.
RUN e2sar_daos_recv --help >/dev/null && e2sar_daos_read --help >/dev/null && e2sar_ft --help >/dev/null

WORKDIR /work
CMD ["e2sar_daos_recv", "--help"]
