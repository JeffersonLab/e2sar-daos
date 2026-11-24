---
type: How-To
title: "Find docs by tag (for agents)"
description: "How an agent finds the right files in this docs bundle by the tags in their YAML headers instead of reading many files: list the tags, match all or any of them, narrow by type, read the description, then open only those files. With scripts/docs_tags.py or plain grep, what the tag groups mean (sites, LB, tools, topics, and the markers todo, analysis, summary, inventory, bug, proposal), example searches for common questions, and the rules for tagging new docs."
tags: [docs, tags, agents, okf, search]
sources:
  - { id: okf, resource: "https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md", title: "OKF v0.2 spec" }
  - { id: agents, resource: "../../AGENTS.md", title: "Agent notes for e2sar-daos" }
---

# Find docs by tag (for agents)

Every file in `docs/` except the `index.md` files and `log.md` starts with a YAML header that has `tags: [...]`. Tags tell you what a file is about (site, LB, tool, topic) and what kind of work it holds (open to-dos, an analysis, a summary). Searching them is cheaper and more precise than reading the folder indexes or grepping the text.

## The steps

1. **See which tags exist** (most used first):

   ```bash
   scripts/docs_tags.py
   ```

2. **Find the files that have all of your tags**, and narrow by `type` if you know it (`Concept`, `How-To`, `Site`, `Reference`, `Result`):

   ```bash
   scripts/docs_tags.py lux loss                 # every file tagged lux AND loss
   scripts/docs_tags.py --type Result lux loss   # only results
   scripts/docs_tags.py --any todo inventory     # tagged todo OR inventory
   ```

   Each line is `path (type [status]) title`.

3. **Pick by description.** `-d` adds each file's tags and description, which is usually enough to choose without opening the file:

   ```bash
   scripts/docs_tags.py -d mtu
   scripts/docs_tags.py --show howto/lb-test.md  # one file's header
   ```

4. **Open only those files.** Check `status` and `stale_after` before you rely on one: `draft` means it hasn't been run yet, and after `stale_after` check the file against the code.

5. **No match?** Run step 1 and look for a nearby tag (e.g. `rate-sweep` and `sweep`, `read` and `reader`). Then fall back to the folder indexes, which list every file with its description, and to `grep -ril <word> docs/`.

`scripts/docs_tags.py` needs only Python 3.8+ (standard library) and finds `docs/` from its own location, so it works from any directory and on the ebpf nodes. Tag matching ignores case.

### Without the script

```bash
grep -rlE '^tags:.*\bmtu\b' docs/                                   # files with one tag
grep -rlE '^tags:.*\blux\b' docs/ | xargs grep -lE '^tags:.*\bloss\b'  # two tags (AND)
grep -rhE '^tags:' docs/ | tr -d '[]' | sed 's/^tags://' | tr ',' '\n' | tr -d ' ' | sort | uniq -c | sort -rn   # every tag
```

`\b` treats `-` as a word boundary, so `\bsweep\b` also matches `rate-sweep`. Use the script when that matters.

## What the tags mean

| Group | Tags | Use for |
|---|---|---|
| **Markers** (what kind of work) | `todo` (has an open checklist), `analysis` (a new look at kept data, no new run), `summary` (spans many runs), `inventory` (what data exists), `bug`, `proposal` | Open work: `--any todo proposal`. Cross-run answers: `summary` |
| **Sites and pools** | `ebpf-testbed`, `slurm-tb`, `perlmutter`, `nersc`, `lux`, `olcf`, `lcf`, `aurora`, `alcf`, `mac`; DAOS pools `sc26` (gone), `sc-demo` | Where something ran. A result is tagged by its **sender's** site as well as the receivers' |
| **Load balancer** | `lb`, `ejfat`, `lbadm`, `two-receivers` | Anything through the ESnet LB |
| **Programs and scripts** | `e2sar_daos_recv`, `e2sar_daos_read`, `e2sar_daos_monitor`, `e2sar_perf`, `e2sar_ft`, `e2sar_evio_send`, `e2sar_e2e_bench`, `e2sar_live_pipeline`, `e2sar_site2ebpf_recv`, `evio-interactive-view` | How to run a tool (`--type How-To` or `Reference`), and the runs that used it |
| **Network and rates** | `loss`, `latency`, `throughput`, `mtu` (checks or sets the path MTU), `rtt`, `bdp`, `nic`, `rx_discards_phy`, `network`, `wan`, `littles-law`, `parallelism`, `sockets`, `upper-bound`, `rate-sweep`, `tuning`, `node-to-node` | Performance questions |
| **Data** | `evio`, `cosmics`, `hit-words`, `sro`, `fadc250`, `compression`, `parquet` | What is in the events |
| **Build and test** | `build`, `meson`, `install`, `rpm`, `testing`, `suites`, `docker`, `podman` | Getting the code to build and run |

This table lists the groups, not every tag. `scripts/docs_tags.py` with no arguments is the current, complete list.

## Example searches

| Question | Search |
|---|---|
| How do I check the path MTU to the LB? | `docs_tags.py --type How-To mtu` |
| What runs came from LUX, and what did they lose? | `docs_tags.py --type Result lux loss` |
| What is the highest rate we've reached? | `docs_tags.py summary`, then `upper-bound` |
| What analysis is still open? | `docs_tags.py todo` |
| What data from old runs still exists? | `docs_tags.py inventory` |
| How do I run the receiver at Perlmutter? | `docs_tags.py perlmutter e2sar_daos_recv`, and the `Site` file |
| Every option of a program | `docs_tags.py --type Reference e2sar_daos_read` |

## Tagging a new or changed doc

- **Reuse existing tags.** Run `scripts/docs_tags.py` first, and don't add a near-duplicate (`sweep` when `rate-sweep` exists).
- **Format:** lowercase, words joined by `-`. Program names exactly as the binary or script (`e2sar_daos_recv`, `e2sar_live_pipeline`). Sites by their short name (`perlmutter`, `lux`).
- **A result** gets: the sites (sender and receivers), `lb` and `ejfat` if it went through the LB, each program it ran, its topics, and a marker if one applies (`todo` if it has an open checklist, `analysis` if it reanalyses kept data).
- **Remove `todo`** when its checklist is done.
- When you add or change tags on existing files, note it in [log.md](../log.md), as for any change.
