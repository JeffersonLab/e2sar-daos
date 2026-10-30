# Reading a run from DAOS: `e2sar_daos_read`

How `e2sar_daos_read` reads the events `e2sar_daos_recv` stored. The objects it reads (catalog, `seconds`, `index[T]`, `data[T, s]`) and the writer–reader contract are in [DAOS_LAYOUT.md](DAOS_LAYOUT.md).

## Reading modes

One binary with three reading modes, each writing events to `-o DIR`. **A mode is required:** give exactly one of `--live`, `--live-rm` or `--offline`; there is no default. All read in `(evtNum, dataId)` order and keep a manifest and a missing-event list in `DIR`.

| Mode | For | Output | Follows the run |
|---|---|---|---|
| `--live` | Streams: `e2sar_evio_send`, coda-fb, `e2sar_perf` | One file per `--interval` seconds, the events glued back to back. DAOS is left as it is | Yes: checks the container every second until the end of the run |
| `--live-rm` | Same, for long runs where DAOS space matters | Same files; objects are removed from DAOS `--rm-after` intervals after their events are in files, and the container is destroyed at the end | Yes, as `--live` |
| `--offline` | File transfers: `e2sar_ft` | One file per `(evtNum, dataId)`, byte for byte the sent event; SHA-256 per file; then the run is deleted from DAOS | No: reads what is committed when it runs, then exits |

The mode is chosen by the user, never guessed from the event bytes. `--offline` excludes `--live` and `--live-rm`, and those two exclude each other; giving none or more than one is an error. Likewise `--interval` is only valid with `--live` and `--live-rm`, `--rm-after` only with `--live-rm`, and `--keep` only with `--offline`.

```bash
e2sar_daos_read --pool <pool> --cont <run> -o DIR --live              # one file per 5 s, DAOS untouched
e2sar_daos_read --pool <pool> --cont <run> -o DIR --live --interval 1 # one file per second
e2sar_daos_read --pool <pool> --cont <run> -o DIR --live-rm           # one file per 5 s; remove from DAOS 10 intervals behind
e2sar_daos_read --pool <pool> --cont <run> -o DIR --live-rm --rm-after 2   # smaller margin, for high rates
e2sar_daos_read --pool <pool> --cont <run> -o DIR --offline           # e2sar_ft: one file per event, SHA-256s, then delete the run
e2sar_daos_read --pool <pool> --cont <run> -o DIR --offline --keep    # same, but keep the run in DAOS
e2sar_daos_read --pool <pool> --cont <run> -o DIR --offline --event 7 --dataid 4321
```

## `--live` and `--live-rm`: one file per `--interval` seconds

```
open the container and read its run record (exit if either is missing: start the writers first)
I = --interval (default 5); W = run start (or --from, or the last bundle in the manifest), rounded down to a multiple of I
every second:
  read the catalog: writer slots, watermarks, done entries
  for each window [W, W+I) whose seconds are all complete (watermarks), oldest first:
    for each second T in the window: list index[T], read the batch tables
    read each batch's bytes: one daos_array_read per (T_data, slot), all ranges in one call
    sort the window's events by (evtNum, dataId)
    write the bundle for window W (below)
    [--live-rm] remove what is now N intervals behind (below)
    W += I
until every claimed slot is done, W is past their final watermarks,
      and no new slot appeared for --end-wait seconds
[--live-rm] destroy the container
```

**The reader checks the catalog every second,** whatever `--interval` is. Writers update their watermarks once a second, so checking more often gains nothing, and checking less often would delay files. A check is a few small reads (one watermark per writer and the done entries). While the reader is behind (started late, or on a finished run), it keeps reading without waiting until it has caught up.

**`--interval` (seconds, default 5)** only sets how many seconds go in one file. Windows are aligned to multiples of the interval (`T − T % I`), so the same run read twice gives the same files. A window's file appears about 1–3 s after the window ends.

For each non-empty window it writes:

```
DIR/<run>_<W>.evio        the window's events glued back to back, sorted by (evtNum, dataId); W = first second
DIR/<run>_<W>.idx         one line per event: evtNum dataId offset size
DIR/<run>_manifest.txt    one line per bundle: <sha256>  <name>  W I events bytes first_evt last_evt
DIR/<run>_missing.txt     event number gaps per dataId, updated as bundles complete
```

- **Glued, no headers.** A bundle is the events' bytes back to back, unchanged: no file header, no trailer, no wrapping. For EVIO-record events (`e2sar_evio_send`, coda-fb) that is the frames in order.
- The last line of the manifest is the restart point: the reader continues with the next window. Restart with the same `--interval`.
- A bundle's content depends only on what is in DAOS, so a reader started during the run and one started after it write the same files with the same checksums.
- Order is strict within a bundle. Across bundles it is close to event number order: an event that committed in a later window than higher-numbered events lands in that later bundle. The manifest's `first_evt last_evt` show each bundle's range, and the reader counts events that are lower than the previous bundle's last as late.
- `--follow` keeps waiting for new writers after the end of the run, instead of exiting after `--end-wait` seconds.

### What `--live-rm` removes

Data stays in DAOS for `--rm-after N` intervals (default 10, so 50 s with the default `--interval 5`) after the reader has written it to a file. If an output file is lost or the reader crashes mid-file, the recent data can still be read again. The margin is measured from the reader's progress, not the clock, so a reader that falls behind never removes data it hasn't written yet.

Let D be the first second of the last window written to a file, flushed, renamed and listed with its SHA-256 in the manifest. Then:

| Object | Removed when |
|---|---|
| `index[T]` and `seconds[T]` | T < D − N × I |
| `data[T, s]` | T < D − N × I, and slot s's `data` watermark is past T (the writer has finished every batch it started in that array) |
| The container (catalog included) | The run has ended: every slot done, every window written, no new slot for `--end-wait` seconds |

- A second the reader passed because a slot stalled (`--stall`), not because its watermark passed, is never removed before the run ends: a late index write from that slot can still land there. It goes when the container is destroyed.
- The data kept in DAOS is about rate × N × I: ~16 GB at 2.5 Gbps and ~500 GB at 80 Gbps with the defaults. Lower `--rm-after` at high rates if the pool is small.
- DAOS frees removed space in the background, shortly after the removal.
- This assumes one reader per run. A second reader of the same run, or `--list` / `--fetch`, only sees what is not yet removed.

## `--offline`: one file per event

```
open the container and read its run record (exit if either is missing)
list seconds, then for each non-empty second T in order:
  list index[T], read the batch tables
  for each event, in (evtNum, dataId) order, not already in the manifest:
    read its bytes (split into reads of at most --max-write for large events)
    write DIR/<run>_<evtNum>_<dataId>.evio, computing its SHA-256 while writing
    print "<sha256>  <file>" and add it to the manifest
if every writer is done, every committed event was written, and neither --event nor --keep was given:
  destroy the container
exit
```

```
DIR/<run>_<evtNum>_<dataId>.evio   one event, byte for byte what the sender sent (for e2sar_ft: the original file)
DIR/<run>.sha256                   one line per file, sha256sum format: <sha256>  <name>  (check with sha256sum -c)
DIR/<run>_manifest.txt             one line per file: <sha256>  <name>  evtNum dataId bytes
DIR/<run>_missing.txt              event number gaps per dataId
```

- **Files are all or nothing.** E2SAR drops an event when any of its packets is lost, so a file with a lost packet never reaches DAOS: it shows up as a gap in `<run>_missing.txt`, and no partial file is written. A file with no loss is restored whole, and its SHA-256 matches the sender's.
- **SHA-256 per file.** Each file's SHA-256 is printed as it is written, in `sha256sum` format, and kept in `DIR/<run>.sha256`. Comparing with the sender's digests needs no second pass over the files.
- **Then the run is deleted from DAOS.** After the last file is written, flushed and renamed, the reader destroys the run's container, so the data lives only in `DIR`. It deletes only when all of these hold; otherwise it keeps the container and says why:
  - every claimed writer slot has `done/<slot>` (no writer can still add events);
  - every committed event was written to `DIR` without error;
  - neither `--event` nor `--keep` was given.
- It reads only what is committed when it runs, so run it after the transfer. If the run isn't finished, it restores what is there and keeps the container; a rerun skips the files already in the manifest.
- `--event N` (with `--dataid D`) restores one event. Finding it reads the index tables of every second: there is no lookup by event number.

## All modes

- Every output file is written as `.tmp`, flushed, then renamed, so other programs only see complete files. A crash leaves at most a `.tmp`, which is removed on restart.
- `--pool` and `-o DIR` are required.

## Inspect, verify, delete

These are used instead of a reading mode (not with one) and write no files:

```bash
e2sar_daos_read --pool <pool> --cont <run> --list                # run settings, writers' final statistics, seconds, events
e2sar_daos_read --pool <pool> --cont <run> --fetch               # verify, print SHA-256 per event
e2sar_daos_read --pool <pool> --cont <run> --delete              # destroy the run's container
```

They see what is committed when they run: they list `seconds` to find the non-empty seconds, then read `index[T]` for each. The reader opens the pool and container read-only, except with `--delete`, `--live-rm`, and `--offline` when it deletes the run at the end.

## Options

| Option | Default | Meaning |
|---|---|---|
| `--pool`, `--cont` | required | pool and the run's container |
| `-o DIR` | required in reading modes | output directory, also holds the manifest (the restart point) |
| `--live`, `--live-rm`, `--offline` | required, exactly one | reading mode |
| `--interval` | 5 | seconds per output file (`--live`, `--live-rm`) |
| `--rm-after` | 10 | intervals kept in DAOS behind the last file written (`--live-rm`) |
| `--keep` | off | don't delete the run after reading (`--offline`) |
| `--from` | run start | first second to read (`--live`, `--live-rm`) |
| `--stall` | 30 | seconds without watermark progress before a writer is skipped |
| `--end-wait` | 10 | seconds with every writer done and no new writer before the end of the run |
| `--follow` | off | never end; keep waiting for new writers |
| `--event`, `--dataid` | all | restore one event (`--offline`) |
| `--list`, `--fetch`, `--delete` | | inspect, verify, delete (no files written) |
