#!/usr/bin/env python3
"""Which characters each font has, written down once at build time.

    python3 tools/fontindex.py assets/fonts build/fonts.idx

libtext picks a font for a character by asking each font in turn whether it
has it. Asking a font means reading it -- and a character no font has walks
the whole list, reading every one, sixteen megabytes of Chinese included, to
learn that none will do. With this index the question is answered from a few
kilobytes, and a font is read only when it is actually going to draw.

The answer comes from the same table FreeType would consult, the font's
cmap: the Unicode subtable, format 12 where there is one (it reaches past
the first 65536 characters, where emoji live) and format 4 otherwise. Every
character that maps to a real glyph counts; one that maps to glyph 0 -- the
box -- does not.

One line per font, the file name and then its ranges in hex:

    NotoSans-Regular.ttf 20-7e a0-377 37a-37f ...
"""
import os, struct, sys


def cmap_of(data):
    num = struct.unpack(">H", data[4:6])[0]
    for i in range(num):
        tag, _, off, length = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        if tag == b"cmap":
            return data[off:off + length]
    return None


def subtables(cmap):
    n = struct.unpack(">H", cmap[2:4])[0]
    for i in range(n):
        pid, eid, off = struct.unpack(">HHI", cmap[4 + 8 * i:12 + 8 * i])
        yield pid, eid, off, struct.unpack(">H", cmap[off:off + 2])[0]


def fmt12(t):
    n = struct.unpack(">I", t[12:16])[0]
    for i in range(n):
        lo, hi, g = struct.unpack(">III", t[16 + 12 * i:28 + 12 * i])
        if g == 0:
            lo += 1          # the first character of the group is the box
        if lo <= hi:
            yield lo, hi


def fmt4(t):
    segs = struct.unpack(">H", t[6:8])[0] // 2
    ends = struct.unpack(">%dH" % segs, t[14:14 + 2 * segs])
    base = 16 + 2 * segs
    starts = struct.unpack(">%dH" % segs, t[base:base + 2 * segs])
    deltas = struct.unpack(">%dh" % segs, t[base + 2 * segs:base + 4 * segs])
    roff_at = base + 4 * segs
    roffs = struct.unpack(">%dH" % segs, t[roff_at:roff_at + 2 * segs])
    for s in range(segs):
        for c in range(starts[s], ends[s] + 1):
            if c == 0xFFFF:
                continue
            if roffs[s] == 0:
                g = (c + deltas[s]) & 0xFFFF
            else:
                at = roff_at + 2 * s + roffs[s] + 2 * (c - starts[s])
                g = struct.unpack(">H", t[at:at + 2])[0]
                if g:
                    g = (g + deltas[s]) & 0xFFFF
            if g:
                yield c, c


def coverage(path):
    with open(path, "rb") as f:
        data = f.read()
    cmap = cmap_of(data)
    if cmap is None:
        return []
    best = None
    for pid, eid, off, fmt in subtables(cmap):
        unicode = pid == 0 or (pid == 3 and eid in (1, 10))
        if not unicode:
            continue
        rank = 2 if fmt == 12 else 1 if fmt == 4 else 0
        if rank and (best is None or rank > best[0]):
            best = (rank, off, fmt)
    if best is None:
        return []
    t = cmap[best[1]:]
    pairs = sorted(fmt12(t) if best[2] == 12 else fmt4(t))
    merged = []
    for lo, hi in pairs:
        if merged and lo <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    return merged


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    lines = []
    for name in sorted(os.listdir(src)):
        if not name.endswith((".ttf", ".otf")):
            continue
        ranges = coverage(os.path.join(src, name))
        if not ranges:
            sys.exit("%s: no Unicode cmap" % name)
        lines.append(name + " " + " ".join("%x-%x" % (a, b) for a, b in ranges))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d fonts, %d bytes" % (out, len(lines), os.path.getsize(out)))


main()
