---
type: Reference
title: "Meson options"
description: "The project's Meson options (meson_options.txt): buildtest, daos_pool and daos_cont, with their defaults and how to change them."
tags: [meson, build, options]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/meson_options.txt", title: "meson_options.txt at 0d85d00" }
---

# Meson options

Defined in [meson_options.txt](../../meson_options.txt).

| Option | Type | Default | What it does |
|---|---|---|---|
| `buildtest` | boolean | `true` | Build the tests (`meson test`) |
| `daos_pool` | string | `sc26` | DAOS pool label for the integration test, passed to it as `DAOS_POOL` |
| `daos_cont` | string | `kv-test` | DAOS container label for the integration test, passed to it as `DAOS_CONT` |

```bash
meson setup build -Dbuildtest=false                            # at setup
meson configure build -Ddaos_pool=mypool -Ddaos_cont=mycont    # on an existing build dir
meson configure build | grep -A1 buildtest                     # show the current value
```

The pool and container must exist before `meson test --suite integration`. See [build](../howto/build.md) and [run-tests](../howto/run-tests.md).
