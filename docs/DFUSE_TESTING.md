# Writing events through dfuse (b2b)

This compares two ways of storing E2SAR events in DAOS on the ebpf testbed:

- **kv**: `e2sar_daos_recv` writes each event into a DAOS KV object with asynchronous puts. This is the existing path.
- **dfuse**: `e2sar_daos_recv --dfuse <mount>` writes each event as one file into a POSIX container mounted with `dfuse`.
- **pil4dfs**: the same as dfuse, with `libpil4dfs.so` preloaded, which turns the file calls into in-process libdfs calls and bypasses FUSE.

All three use the back-to-back (b2b) bench, `bin/e2sar_e2e_bench.sh -m b2b`: `e2sar_evio_send` on ebpf2201 sends synthetic EVIO events straight to the receiver on ebpf2203, with no load balancer.
Setup, MTU and NIC notes are in [EBPF_TESTBED_NOTES.md](EBPF_TESTBED_NOTES.md).

```
ebpf2201                                    ebpf2203
e2sar_evio_send ──UDP :20700+i──▶ e2sar_daos_recv ─┬─ kv:      DAOS KV object in sc26/e2sar-dfuse
129.57.178.85                     129.57.178.86    ├─ dfuse:   /tmp/$USER-dfuse/<TAG>/<TAG>_<evt>_<dataId>.evio
                                                   └─ pil4dfs: same files, FUSE bypassed
```

## 1. What the dfuse sink does

`e2sar_daos_recv --dfuse DIR` skips the DAOS KV setup and writes `DIR/<prefix>/<prefix>_<evt>_<dataId>.evio` for each event, using a blocking `open`/`write`/`close` on each `--deq` thread.
The file names are the same ones `e2sar_daos_read --fetch -o` produces.
An event counts as committed once `close()` returns. Add `--fsync` to also `fsync()` each file before closing it.
`--pool`, `--cont`, `--oid`, `--oclass`, `--chunk` and `--inflight` are ignored in dfuse mode, since the mount decides where the data goes.

The bench script adds two receiver options:

| Option | Meaning |
|---|---|
| `-S kv\|dfuse\|pil4dfs` | Sink. Default `kv` |
| `-D N` | Dequeue threads (`e2sar_daos_recv --deq`), the same option as for the kv sink. For `dfuse`/`pil4dfs` each one is a blocking file writer. The default is 16 for every sink ([E2E_BENCHMARK.md](E2E_BENCHMARK.md#3-dequeue-threads-the-hidden-bottleneck)) |
| `-I N` | In-flight puts per dequeue thread (`--inflight`), kv only. Default 128 |

For `dfuse` and `pil4dfs`, the receiver creates `$CONT` as a POSIX container, mounts it at `$MNT`, and starts `e2sar_daos_recv --dfuse $MNT`.
After the run it checks every file with `sha256sum` against the sender's digests, then unmounts and destroys the container. This also happens if the run fails.

### Settings that make dfuse faster

| Variable | Default | Why |
|---|---|---|
| `DFUSE_OPTS` | `--thread-count=32 --eq-count=8` | dfuse uses at most 16 threads and one event queue by default, which limits parallel writers. Set `DFUSE_OPTS=` to get dfuse's own defaults |
| `-S pil4dfs` | off | Preloads `/usr/lib64/libpil4dfs.so` (`PIL4DFS`), so reads and writes skip FUSE and the kernel |
| `PIL4DFS_ENV` | `D_IL_MAX_EQ=0` | **Required with more than 2 writer threads.** With pil4dfs's default per-thread event queues, the receiver segfaults in libfabric (`ibv_get_async_event` ← `rxm_conn_progress`) on the first events. `D_IL_MAX_EQ=0` makes pil4dfs use blocking calls |

## 2. Before you start

- Check that nobody else is using the nodes. Other benchmarks use ports 20480 and 20600 (containers `e2sar-test`, `e2sar-bench`, `e2sar-lb`). Runs at the same time skew each other's numbers, and the NIC drop counters are node-wide.
  ```bash
  pgrep -af 'e2sar_e2e_bench|e2sar_daos_recv|e2sar_evio_send|e2sar_ft'    # on both nodes
  ss -uan | grep -E ':20(48|6|7)[0-9]'
  ```
- Avoid 03:00–03:30, when the nightly iperf3 job runs.
- This doc uses port 20700, container `e2sar-dfuse`, and `RUN=$HOME/e2sar_run/dfuse`, so its logs and `results.jsonl` stay apart from the other benchmarks.
- dfuse runs as your user. `/dev/fuse` is world-writable on both nodes, and `fusermount3` unmounts. No root is needed.

## 3. Build (once, on either node)

The branch lives in its own worktree, so the main checkout `~/daq/e2sar-daos` and its build stay untouched.

```bash
bash
cd ~/daq/e2sar-daos
git fetch https://github.com/JeffersonLab/e2sar-daos.git feature/dfuse-bench
git worktree add ~/daq/e2sar-daos-dfuse FETCH_HEAD          # or: cd ~/daq/e2sar-daos-dfuse && git checkout --detach FETCH_HEAD
cd ~/daq/e2sar-daos-dfuse
export PATH=$HOME/.local/bin:$PATH
export E2SAR_ROOT=$HOME/opt/e2sar
export PKG_CONFIG_PATH=$E2SAR_ROOT/usr/local/lib64/pkgconfig:$E2SAR_ROOT/usr/local/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$E2SAR_ROOT
export BOOST_ROOT=$E2SAR_ROOT/usr/local
meson setup build && meson compile -C build
meson test -C build --suite unit                            # Ok: 3, Fail: 0
./build/bin/e2sar_daos_recv --help | grep -A1 -- --dfuse
```

The script finds the build next to itself (`bin/../build`), so always run the worktree's copy of `bin/e2sar_e2e_bench.sh`.

## 4. Smoke test: one segmenter, 300 events, 2 Gbps

This takes a few seconds per sink and loads the network and DAOS only lightly.
Start the receiver first. The sender waits up to 180 s for it, so the order of the two commands doesn't matter much, but both must use the same `TAG`.

**Both nodes:**
```bash
bash
export PORT=20700 CONT=e2sar-dfuse RUN=$HOME/e2sar_run/dfuse
B=~/daq/e2sar-daos-dfuse/bin/e2sar_e2e_bench.sh
S=dfuse                                    # or pil4dfs, or kv
TAG=dfsmoke_${S}_<same number on both nodes>
```

**ebpf2203, receiver:**
```bash
$B recv $TAG -m b2b -T 1 -S $S
```

**ebpf2201, sender:**
```bash
$B send $TAG -m b2b -P 1 -s 1 -n 300 -f 4096 -r 2
```

The receiver prints a summary and exits 0 on PASS:
```
dfsmoke_dfuse_1792444835 PASS  mode=b2b P=1 s=1 f=4096 rate=2 T=1 D=16 sink=dfuse  committed 300/300, verified 300
  received 2.04 Gbps (168 evt/s, 1521898 B/event)  e2e 2.00 Gbps  sender 2.00 Gbps
  ...
  latency daos     p50     9.138  p90    28.656  p99    53.733  max 82.778 ms
```
PASS means every event was committed and every file's SHA-256 matches the sender's digest.
For the dfuse sinks, the `daos` latency row is the time from the reassembled event to `close()` returning.

## 5. Throughput comparison

Raise the rate and the number of segmenters, and run the same sweep for each sink. The receiver needs at least as many ports as the sender has segmenters (`-T >= -P`).

```bash
STAMP=<same number on both nodes>
# ebpf2203
for S in kv dfuse pil4dfs; do for R in 5 10 20 30; do
  $B recv tput_${S}_r${R}_$STAMP -m b2b -T 32 -S $S
done; done
# ebpf2201, same order and TAGs
for S in kv dfuse pil4dfs; do for R in 5 10 20 30; do
  $B send tput_${S}_r${R}_$STAMP -m b2b -P 4 -s 1 -n 2500 -f 4096 -r $R
done; done
```
`-r` is the total sender rate in Gbps. Each run sends 4 × 2500 = 10,000 events of about 1.5 MB (15 GB).
Use `-D` to change the number of dequeue (writer) threads (for example `-D 32`), and `DFUSE_OPTS` to try other dfuse settings. Both are recorded in each run's JSON line.

Results go to `$RUN/bench/<TAG>/` (logs, `timing.csv`, `fetch.txt`, `result.json`) and one JSON line per run to `$RUN/bench/results.jsonl`. To compare sinks:
```bash
python3 - <<'EOF'
import json, os
for l in open(os.path.expanduser("~/e2sar_run/dfuse/bench/results.jsonl")):
    r = json.loads(l)
    print(f"{r['tag']:36s} {r['result']} sink={r.get('sink','kv'):8s} D={r.get('deq')} "
          f"rate={r['rate']} received={r.get('received_gbps',0):6.2f} Gbps "
          f"commit p50={r.get('latency_ms',{}).get('daos',{}).get('p50')} ms")
EOF
```

## 6. Clean up

The receiver unmounts and destroys `e2sar-dfuse` after every run, including failed ones, and writes nothing to `/scratch`.
If a run was killed with `kill -9` or a power cut, clean up by hand:

```bash
PORT=20700 CONT=e2sar-dfuse $B clean            # unmounts $MNT if mounted, destroys the container
mount | grep dfuse; daos cont list sc26         # neither should show e2sar-dfuse
```
Keep `$RUN`, which holds the logs and results.

## 7. Results from 2026-10-19 (smoke test)

ebpf2201 → ebpf2203, b2b, MTU 9000, 1 segmenter at 2 Gbps, 300 events of about 1.5 MB, 1 receive port.
Another b2b sweep was running on port 20600 at the same time, so these show correctness and per-event write cost, not peak throughput.

| Sink | Writer threads | Result | Received | Write latency p50 / p90 / p99 |
|---|---|---|---|---|
| kv | 4 (128 in-flight puts each) | PASS 300/300 | 2.03 Gbps | 1.0 / 12.9 / 22.6 ms |
| dfuse (`-t 32 -e 8`) | 16 | PASS 300/300 | 2.04 Gbps | 9.1 / 28.7 / 53.7 ms |
| pil4dfs (`D_IL_MAX_EQ=0`) | 16 | PASS 300/300 | 2.02 Gbps | 1.0 / 1.1 / 13.1 ms |

Through FUSE, each 1.5 MB file costs about 9 ms. With pil4dfs it costs about 1 ms, the same as the native KV path.
A single `dd` stream (4 MiB blocks, `conv=fsync`) into the dfuse mount ran at 446 MB/s.
The throughput sweep (step 5) still needs an idle testbed.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `pil4dfs` run: `Segmentation fault (core dumped)`, `committed 0/N` | pil4dfs per-thread event queues crash in libfabric with more than ~2 writer threads. Keep `PIL4DFS_ENV=D_IL_MAX_EQ=0` (the default) |
| `ERROR: /tmp/<user>-dfuse is already mounted` | A previous run didn't finish. Run `$B clean` with the same `CONT`, after checking that no other run is using the mount |
| `dfuse mount ... failed` | Check `daos pool query sc26` and that `daos_agent` is running. Run `dfuse -f $MNT sc26 e2sar-dfuse` in the foreground to see the error |
| `fusermount3: ... Device or resource busy` | Something still has a file open under the mount (for example a shell `cd`'d into it). Leave the directory and retry |
| `ERROR: UDP port 20700 is in use` | Another receiver is listening there. Check `ss -uanp`, or choose another `PORT` on both nodes |
| PASS but `rx_discards_phy` > 0 | The counter is for the whole NIC and also counts other traffic, such as another benchmark |
