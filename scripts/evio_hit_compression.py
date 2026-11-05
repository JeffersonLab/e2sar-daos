#!/usr/bin/env python3
"""How far EVIO time frames of fADC250 hits compress, by method.

Reads the first N records of an EVIO v6 file or an e2sar_daos_read --live
bundle (bare records), pulls out the hit words, and prints the size ratio
(raw EVIO bytes / compressed bytes) and single-thread speed of:
general codecs on the raw bytes, per-frame compression, Parquet, and
hit-aware columns (time step within a port bank, channel, charge, count).

Needs numpy and pyarrow (evio-interactive-view's .venv has both).

    evio_hit_compression.py FILE [N_RECORDS=20000] [--tmp DIR]
"""
import argparse, os, time
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq


def read_hits(path, nrec):
    raw = np.memmap(path, dtype=np.uint8, mode='r')
    # bare records (--live bundle) or a file with a 14-word file header first
    for skip in (0, 14):
        for dt in ('>u4', '<u4'):
            w = raw.view(dt)[skip:]
            if w[7] == 0xC0DA0100:
                break
        else:
            continue
        break
    else:
        raise SystemExit(f"{path}: no EVIO v6 record header found")
    pos, recs, events = 0, 0, []
    hits, frames, ports = [], [], []
    while pos < len(w) and recs < nrec:
        rl, hl = int(w[pos]), int(w[pos + 2])
        il, ul = int(w[pos + 4]) // 4, (int(w[pos + 6]) + 3) // 4
        e = pos + hl + il + ul
        end = e + int(w[e]) + 1
        events.append((e, end))
        p, fr = e + 2, recs
        while p < end:
            L, tag = int(w[p]), int(w[p + 1]) >> 16
            if tag == 0xFF31:
                fr = int(w[p + 3])
            elif tag < 0xFF00:                       # ROC bank
                q, qe = p + 2, p + 1 + L
                while q < qe:
                    L2, t2 = int(w[q]), int(w[q + 1]) >> 16
                    if t2 != 0xFF30 and L2 > 1:      # payload bank with hits
                        n = L2 - 1
                        hits.append(np.asarray(w[q + 2:q + 2 + n], dtype=np.uint32))
                        frames.append(np.full(n, fr, np.uint32))
                        ports.append(np.full(n, t2, np.uint8))
                    q += L2 + 1
            p += L + 1
        pos += rl
        recs += 1
    off = skip * 4                                    # bytes of the file header, if any
    rawbytes = bytes(raw[off:off + pos * 4])
    evbytes = [bytes(raw[off + a * 4:off + b * 4]) for a, b in events]
    return rawbytes, evbytes, np.concatenate(hits), np.concatenate(frames), np.concatenate(ports)


def entropy(x):
    _, c = np.unique(x, return_counts=True)
    p = c / c.sum()
    return float(-(p * np.log2(p)).sum())


def shuffle(v):
    v = np.ascontiguousarray(v)
    return np.ascontiguousarray(v.view(np.uint8).reshape(-1, v.itemsize).T).tobytes()


def timed(fn):
    t0 = time.perf_counter()
    out = fn()
    return out, time.perf_counter() - t0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file')
    ap.add_argument('nrec', nargs='?', type=int, default=20000)
    ap.add_argument('--tmp', default='/tmp', help='where to write the Parquet test files (removed after)')
    a = ap.parse_args()

    raw, evs, H, F, P = read_hits(a.file, a.nrec)
    N = len(raw)
    t = ((H >> 17) & 0x3FFF).astype(np.int32)
    ch = ((H >> 13) & 0xF).astype(np.uint8)
    q = (H & 0x1FFF).astype(np.uint16)
    print(f"frames {len(evs)}, {N / 1e6:.1f} MB, {N / len(evs):.0f} B/frame, "
          f"{len(H) / len(evs):.0f} hits/frame, hit words {4 * len(H) / N:.1%} of bytes")

    newb = np.ones(len(H), bool)
    newb[1:] = (F[1:] != F[:-1]) | (P[1:] != P[:-1])
    dt = np.diff(t, prepend=0)
    dt[newb] = t[newb]
    cnt = np.diff(np.flatnonzero(np.r_[newb, True])).astype(np.uint16)
    print(f"entropy, bits per hit: t {entropy(t):.2f}, dt within port bank {entropy(dt):.2f} "
          f"({(dt[~newb] < 0).mean():.1%} steps back), ch {entropy(ch):.2f}, q {entropy(q):.2f}; "
          f"bound dt+ch+q {entropy(dt) + entropy(ch) + entropy(q):.1f} of 32")

    def row(name, nbytes, secs=None, src=N):
        speed = f"  {src / secs / 1e6:6.0f} MB/s" if secs else ""
        print(f"  {name:48} {src / nbytes:5.2f}x{speed}")

    print("ratio = raw EVIO bytes / compressed bytes; speed = one thread")
    for codec, lvl in [('lz4', None), ('zstd', 1), ('zstd', 3), ('zstd', 9)]:
        c = pa.Codec(codec, compression_level=lvl)
        out, s = timed(lambda: c.compress(raw, asbytes=True))
        row(f"raw EVIO, {codec}{lvl or ''}", len(out), s)
    k = evs[:5000]
    src = sum(map(len, k))
    for codec, lvl in [('lz4', None), ('zstd', 1), ('zstd', 3)]:
        c = pa.Codec(codec, compression_level=lvl)
        out, s = timed(lambda: sum(len(c.compress(e, asbytes=True)) for e in k))
        row(f"each frame on its own, {codec}{lvl or ''} ({len(k)} frames)", out, s, src)
    z = pa.Codec('zstd', compression_level=3)
    row("hit words only, zstd3", len(z.compress(H.astype('>u4').tobytes(), asbytes=True)))

    fn = os.path.join(a.tmp, f"xmei_hitcomp_{os.getpid()}.parquet")
    o = np.lexsort((t, F))
    cases = [
        ("Parquet zstd, sorted by (frame, t)", o, {}),
        ("Parquet zstd, stream order", slice(None), {}),
        ("Parquet zstd, stream order, delta t", slice(None),
         dict(use_dictionary=['frame', 'port', 'ch'],
              column_encoding={'t_ns': 'DELTA_BINARY_PACKED', 'q': 'PLAIN'})),
    ]
    for name, idx, kw in cases:
        tbl = pa.table({'frame': F[idx], 'port': P[idx], 'ch': ch[idx],
                        't_ns': (t[idx] * 4).astype(np.int32), 'q': q[idx].astype(np.int32)})
        try:
            _, s = timed(lambda: pq.write_table(tbl, fn, compression='zstd', row_group_size=1 << 20, **kw))
            row(name, os.path.getsize(fn), s)
        finally:
            if os.path.exists(fn):
                os.remove(fn)

    for lvl in (1, 3):
        c = pa.Codec('zstd', compression_level=lvl)
        out, s = timed(lambda: sum(len(c.compress(shuffle(v), asbytes=True))
                                   for v in (dt.astype(np.int16), ch, q, cnt)))
        row(f"hit columns dt/ch/q/count, shuffled, zstd{lvl}", out, s)


if __name__ == '__main__':
    main()
