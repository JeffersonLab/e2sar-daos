#!/usr/bin/env python3
"""
evio_concat.py OUT IN1 IN2 ...: join EVIO v6 files into one EVIO v6 file.
evio_concat.py --fix-last-bit FILE: only fix the last-record bits of FILE, in place.

A plain `cat` doesn't give an EVIO file: every input has its own file header (and maybe
an index array, user header and trailer), which would end up in the middle. This keeps
the first file's header, then copies the records of every input in order, in bulk,
without their file headers, index arrays, user headers or trailers. In the output's file
header the record count is the total, and the index array and the trailer are dropped
(trailer position 0, the "trailer with index" bit cleared). The first file's user header
(e.g. a dictionary) is kept. Record numbers inside the record headers are not changed,
so they restart with each input.

Some writers set the "last record" bit (bit 9 of the record header's bit info word) on
every record. A reader that stops at that bit would read only the first record, so the
output has it set on its last record only. That changes 4 header bytes per record and
no event bytes.

All inputs must have the same byte order. Prints the records per input and in total.
"""

import mmap
import os
import struct
import sys

MAGIC = 0xC0DA0100
FILE_HDR_WORDS = 14
TRAILER = 3          # record header type (bit info bits 28-31) of a trailer
TRAILER_INDEX_BIT = 1 << 10
LAST_RECORD_BIT = 1 << 9


def fix_last_bits(path):
    """Clear the last-record bit on every record but the last, set it on the last. In place."""
    with open(path, "r+b") as f, mmap.mmap(f.fileno(), 0) as mm:
        bo = ">" if struct.unpack_from(">I", mm, 28)[0] == MAGIC else "<"
        w = struct.unpack_from(bo + "14I", mm, 0)
        pos = w[2] * 4 + w[4] + (w[6] + 3) // 4 * 4
        size = len(mm)
        word = struct.Struct(bo + "I")
        records = cleared = 0
        last = None
        while pos + 56 <= size:
            if word.unpack_from(mm, pos + 28)[0] != MAGIC:
                sys.exit(f"{path}: bad record header at byte {pos}")
            info = word.unpack_from(mm, pos + 20)[0]
            if (info >> 28) & 0xF == TRAILER:
                break
            if info & LAST_RECORD_BIT:
                word.pack_into(mm, pos + 20, info & ~LAST_RECORD_BIT)
                cleared += 1
            last = pos
            records += 1
            pos += word.unpack_from(mm, pos)[0] * 4
        if last is not None:
            word.pack_into(mm, last + 20, word.unpack_from(mm, last + 20)[0] | LAST_RECORD_BIT)
        mm.flush()
    print(f"{path}: {records} records; last-record bit cleared on {cleared - (1 if last is not None else 0)}, "
          f"set on the last")


def file_layout(path):
    """(byte order, header bytes, first record offset, end of records, records) of one file."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        hdr = f.read(FILE_HDR_WORDS * 4)
        for bo in (">", "<"):
            w = struct.unpack(bo + "14I", hdr)
            if w[7] == MAGIC:
                break
        else:
            sys.exit(f"{path}: no EVIO magic number in the file header")
        if w[5] & 0xFF != 6:
            sys.exit(f"{path}: not EVIO version 6")
        user = (w[6] + 3) // 4 * 4
        first = w[2] * 4 + w[4] + user
        trailer_pos = w[10] << 32 | w[11] if bo == ">" else w[11] << 32 | w[10]
        if trailer_pos:  # the header says where the records end and how many there are
            f.seek(trailer_pos)
            r = struct.unpack(bo + "14I", f.read(56))
            if r[7] != MAGIC or (r[5] >> 28) & 0xF != TRAILER:
                sys.exit(f"{path}: no trailer at byte {trailer_pos}, as the file header says")
            return bo, hdr, w, first, trailer_pos, w[3]
        # no trailer position: walk the record headers up to the trailer (or the end of the file)
        pos, records = first, 0
        while pos + 56 <= size:
            f.seek(pos)
            r = struct.unpack(bo + "14I", f.read(56))
            if r[7] != MAGIC:
                sys.exit(f"{path}: bad record header at byte {pos}")
            if (r[5] >> 28) & 0xF == TRAILER:
                break
            pos += r[0] * 4
            records += 1
        if trailer_pos and trailer_pos != pos:
            sys.exit(f"{path}: the trailer is at {pos}, the file header says {trailer_pos}")
        return bo, hdr, w, first, pos, records


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--fix-last-bit":
        fix_last_bits(sys.argv[2])
        return
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, inputs = sys.argv[1], sys.argv[2:]
    layouts = [file_layout(p) for p in inputs]
    bo = layouts[0][0]
    if any(l[0] != bo for l in layouts):
        sys.exit("inputs differ in byte order")
    total = sum(l[5] for l in layouts)

    # the first file's header and user header, with the totals and without index and trailer
    _, hdr, w, first, _, _ = layouts[0]
    w = list(w)
    w[3] = total
    w[4] = 0
    w[5] &= ~TRAILER_INDEX_BIT
    w[10] = w[11] = 0
    with open(inputs[0], "rb") as f:
        f.seek(w[2] * 4 + layouts[0][2][4])  # past the header and the first file's index array
        user = f.read((w[6] + 3) // 4 * 4)

    tmp = out + ".tmp"
    with open(tmp, "wb") as o:
        o.write(struct.pack(bo + "14I", *w))
        o.write(user)
        for path, (_, _, _, start, end, records) in zip(inputs, layouts):
            with open(path, "rb") as f:
                f.seek(start)
                left = end - start
                while left:
                    buf = f.read(min(left, 64 << 20))
                    o.write(buf)
                    left -= len(buf)
            print(f"{path}: {records} records, {end - start} bytes")
        o.flush()
        os.fsync(o.fileno())
    fix_last_bits(tmp)
    os.rename(tmp, out)
    print(f"{out}: {total} records, {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
