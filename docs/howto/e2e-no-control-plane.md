---
type: How-To
title: "End-to-end example (no control plane)"
description: "Send events with e2sar_perf straight to e2sar_daos_recv, check them with e2sar_daos_read, view restored EVIO files over an SSH tunnel, and clean up."
tags: [e2e, e2sar_perf, e2sar_daos_read, evio]
sources:
  - { id: origin, resource: "https://github.com/JeffersonLab/e2sar-daos/blob/0d85d00/bin/README.md", title: "bin/README.md at 0d85d00" }
---

# End-to-end example (no control plane)

On the receiver node (`192.168.1.20`), with DAOS:

```bash
export EJFAT_URI='ejfat://useless@10.0.0.1:18347/lb/1?data=192.168.1.20&sync=192.168.1.20:19020'
./build/bin/e2sar_daos_recv --ip 192.168.1.20 --port 19522 --pool sc26 --cont-name run42
```

On the sender node, using the same `EJFAT_URI`, send 100 events of 1 MiB at 5 Gbps with `e2sar_perf`:

```bash
e2sar_perf -s --ip <sender ip> --mtu 9000 -l 1048576 -n 100 --rate 5
```

- `-l` is the event size in bytes, `-n` the number of events and `--rate` the send rate in Gbps. Use `--mtu 1500` if the path doesn't pass 9000-byte packets.
- `e2sar_perf` numbers the events 0 to N−1 on every send, so use a new container (`--cont-name`) for each send.
- Every event is the same buffer: `This is a start of event payload` at its start and `...the end` at its end. Don't pass `--realmalloc`, which breaks the single-digest check below.

Stop the receiver with Ctrl-C once the sender finishes. Then check the data on any DAOS client node:

```bash
./build/bin/e2sar_daos_read --pool sc26 --cont run42 --list | tail -1     # 100 committed events, 104857600 bytes
./build/bin/e2sar_daos_read --pool sc26 --cont run42 --fetch | grep -E '^[0-9a-f]{64} ' | cut -d' ' -f1 | sort | uniq -c
#   100 <digest>          one line: all 100 events are the same buffer, intact
```

Fewer than 100 events means some were lost on the way (the receiver's `Lost in reassembly` shows how many); more than one digest means an event was corrupted. To look inside one event:

```bash
./build/bin/e2sar_daos_read --pool sc26 --cont run42 -o ~/restored --offline --event 0   # ~/restored/run42_0_<dataId>.evio
head -c 32 ~/restored/run42_0_*.evio; echo                 # This is a start of event payload
```

To store real files, one event per file (e.g. EVIO files you can open in a viewer), send them with `e2sar_ft -s --ip <sender ip> --rate 5 /path/to/files/*.evio`, then restore them with `--offline` and compare `sha256sum` of the originals with `~/restored/run42.sha256`.

Clean up after every test run, once it is verified:

```bash
rm -rf ~/restored
./build/bin/e2sar_daos_read --pool sc26 --cont run42 --delete
```
## Viewing the restored files

Events sent from EVIO files with `e2sar_ft` restore as ordinary `.evio` files, so any EVIO viewer opens them. (`e2sar_perf` events are not EVIO.) They are on the node where `e2sar_daos_read -o` ran, usually a remote DAOS client. To browse them from your laptop, run the [evio-interactive-view](https://code.jlab.org/xmei/evio-interactive-view) web viewer on that node and forward its port over SSH. See its README for setup (a Python venv with `pip install -r requirements.txt`).

```bash
# on the DAOS client node, in the evio-interactive-view checkout
python serve.py ~/restored --no-browser
# on your laptop
ssh -L 8765:localhost:8765 user@daos-client
```

Then open http://localhost:8765 on your laptop and click a `run42_<evt>_<dataId>.evio` file in the folder browser.

- **Another port:** if port 8765 is taken on the node, start with `--port 9000` and forward `-L 8765:localhost:9000`.
- **Tunnel only:** the server listens on 127.0.0.1 only, so the SSH tunnel is the only way to reach it.
- **The file is sent to your laptop:** decoding runs in the browser, so the whole file goes through the tunnel.
- **Size limit:** files must be smaller than 2 GiB. Split larger events with its `evio_split.py` first.

Delete the restored files when you are done (see the cleanup above).
