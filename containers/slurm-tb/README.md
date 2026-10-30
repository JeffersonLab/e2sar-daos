## The e2sar-daos runtime container for the Slurm TB

**This image is for the JLab Slurm TB nodes.** The nodes run AlmaLinux 9 with `daos_agent` and access to pool `sc26`, but have no `daos-devel` and no E2SAR. The image adds what's missing.

It is the [Rocky 9 dev image](../rocky-dev/) with e2sar-daos built and installed into `/usr/local`. Everything is on `PATH`:

| Binary | From | Role |
|---|---|---|
| `e2sar_daos_recv` | this repo | receive E2SAR events and write them to DAOS (async KV puts) |
| `e2sar_daos_read` | this repo | list, verify (SHA-256) and restore what was written |
| `e2sar_ft` | E2SAR RPM | send files (`-s`) from another node |

Option reference and the DAOS data layout are in [bin/README.md](../../bin/README.md). For podman setup on the nodes (node-local image storage, the agent socket, SELinux, InfiniBand), see [docs/slurm-tb §3](../../docs/sites/slurm-tb.md#3-slurm-tb-nodes-daos-pool-but-no-daos-devel). Everything there applies here too.

---

### Build the image

The base image comes first. Then build this one, **from the repo root**, because it copies in the whole source tree:

```bash
podman build -t e2sar-daos:dev containers/rocky-dev                     # base, once
podman build -f containers/slurm-tb/Dockerfile -t e2sar-daos:tb .        # rebuild after code changes
```

To use it on the TB nodes, save it to the shared filesystem and load it on each node, as in [docs/sites/slurm-tb.md](../../docs/sites/slurm-tb.md):
```bash
podman save e2sar-daos:tb | gzip > /path/on/shared/fs/e2sar-daos-tb.tar.gz
# on the TB node:
podman load -i /path/on/shared/fs/e2sar-daos-tb.tar.gz
```

### Common `podman run` flags

Both the sender and the receiver use the host network. The receiver also needs the DAOS agent socket and your uid:

```bash
TB_RUN="podman run --rm -it --network host --userns keep-id --security-opt label=disable \
        -v /var/run/daos_agent:/var/run/daos_agent -v $PWD:/work -w /work"
# InfiniBand DAOS fabric: add  --device /dev/infiniband --ulimit memlock=-1:-1
```

### End-to-end run: e2sar_ft on node A → DAOS via node B

Without a control plane, `EJFAT_URI` only has to carry the receiver's data address (and a sync address). The sender uses data port 19522 by default, so the receiver listens there. One run is one DAOS container, so pick a new name per run:

```bash
export EJFAT_URI='ejfat://useless@10.0.0.1:18347/lb/1?data=<B ip>&sync=<B ip>:19020'
export CONT=run_$(date -u +%s)          # this run's container; the same on every node
```

**Node B (receiver)**, e.g. from `srun -p <tb-partition> -N1 --pty bash`:
```bash
$TB_RUN -e EJFAT_URI e2sar-daos:tb \
    e2sar_daos_recv --ip <B ip> --port 19522 --pool sc26 --cont-name $CONT --deq 2
```
The container `sc26/$CONT` is created. A progress line is printed every second. Stop with Ctrl-C after the sender is done, and the outstanding writes are flushed before exit.

**Node A (sender)**, with the files on the shared filesystem:
```bash
$TB_RUN -e EJFAT_URI -v /path/to/files:/data:ro e2sar-daos:tb \
    e2sar_ft -s --ip <A ip> --rate 5 /data/*.evio
sha256sum /path/to/files/*.evio
```

**Any node (check):**
```bash
$TB_RUN e2sar-daos:tb e2sar_daos_read --pool sc26 --cont $CONT --list     # list committed events
$TB_RUN e2sar-daos:tb e2sar_daos_read --pool sc26 --cont $CONT --fetch    # SHA-256 per event
$TB_RUN e2sar-daos:tb e2sar_daos_read --pool sc26 --cont $CONT -o /work/restored --offline   # ./restored/${CONT}_<evt>_<dataId>.evio
```
The digests (also in `restored/$CONT.sha256`) should be the same set as the `sha256sum` output. Event numbers follow the order in which `e2sar_ft` queued the files.
The restored `.evio` files are the original files byte for byte, ready for an EVIO viewer.

**Clean up after every run**, once it is verified:
```bash
rm -rf restored                                      # restored files on the node that ran e2sar_daos_read
$TB_RUN e2sar-daos:tb e2sar_daos_read --pool sc26 --cont $CONT --delete   # this run's container
# sender: remove staged copies made for the test, never the original files under /path/to/files
```

### Notes

- Use a new `--cont-name` per run. `e2sar_ft` restarts event numbers at 0, so a second send into the same container repeats event numbers.
- The image's DAOS client is 2.6. Keep it on the same minor version as the nodes' `daos-client` (`rpm -q daos-client`).
- At exit, `e2sar_daos_recv` prints a `Timing:` block with ingest and DAOS commit throughput and write latencies. Add `--timing-csv /work/timing.csv` for per-event rows ([details](../../docs/reference/e2sar_daos_recv.md#timing-and-throughput)).
- The progress line's `enqueue loss` counts events dropped because DAOS writes didn't keep up. Try more `--deq` threads or a higher `--inflight` first. Then lower the sender `--rate`.
