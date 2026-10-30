---
type: Site
title: "Mac without DAOS"
description: "Build and run the unit suite on a Mac in the Rocky 9 dev container under linux/amd64 emulation."
tags: [mac, docker, rocky-dev]
stale_after: 2027-01-31T00:00:00Z
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/docs/TESTING.md", title: "TESTING.md at 0d85d00" }
---

# 1. Mac without DAOS

DAOS and E2SAR only ship x86_64 Linux packages, so on a Mac the build runs in the Rocky 9 dev container ([containers/rocky-dev](../../containers/rocky-dev/)) under `linux/amd64` emulation.
That container has DAOS 2.6 client libraries and headers and a prebuilt E2SAR, but no DAOS server.
So it checks that everything compiles and links, and runs the `unit` suite.
It cannot run `DAOSKVTest`.

```bash
cd e2sar-daos

# Build the image (once, or after the Dockerfile changes).
# On JLab networks, first add the JLab CA certs to containers/rocky-dev/certs/.
docker build --platform linux/amd64 -t e2sar-daos:dev containers/rocky-dev

# Configure, compile and run the unit suite.
# The build dir is inside the container (/tmp/b) so it doesn't clash with a host build/.
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work e2sar-daos:dev \
    bash -c "meson setup /tmp/b && meson compile -C /tmp/b && meson test -C /tmp/b --suite unit -v"
```

Check that:
- `meson setup` does **not** print `libcrypto (openssl-devel) not found, skipping DAOSKVTest`, so the KV test is at least compiled.
- `meson compile` links both `daos_test` and `daos_kv_test`.
- `--suite unit` reports `Ok: 5`.

If `meson setup` fails with `Could not generate cargs for e2sar` or `Package 're2' ... not found`, the image was built from an older Dockerfile. Rebuild it.

---
