# e2sar-daos scripts

Bash and Python scripts that drive the [applications](../bin/README.md) on the JLab ebpf nodes. Each one prints its usage when run without arguments, and the comment at its top lists its settings.

| Script | What it does |
|---|---|
| [`e2sar_e2e_bench.sh`](e2sar_e2e_bench.sh) | One end-to-end run: `e2sar_evio_send` → [LB] → `e2sar_daos_recv` → DAOS, with throughput, latency per stage and a digest check |
| [`e2sar_e2e_sweep.sh`](e2sar_e2e_sweep.sh) | A rate sweep of `e2sar_e2e_bench.sh` from a machine that can SSH to both nodes |
| [`e2sar_layout_sweep.py`](e2sar_layout_sweep.py) | The tuning sweep of `e2sar_daos_recv`, one JSON line per run |
| [`e2sar_site2ebpf_recv.sh`](e2sar_site2ebpf_recv.sh) | Runs an ebpf node as a receiver for a remote sender (Perlmutter or another site) |

Where each runs, the environment they need, and examples: [docs/reference/scripts.md](../docs/reference/scripts.md).
