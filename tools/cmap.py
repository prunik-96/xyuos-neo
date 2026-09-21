#!/usr/bin/env python3
"""Read a TrueType cmap directly -- enough of it to answer one question: which
characters does this face actually have? Format 4 is what every desktop font
uses for the basic plane."""
import struct, sys

d = open("/tmp/font.ttf", "rb").read()

numTables = struct.unpack(">H", d[4:6])[0]
tables = {}
for i in range(numTables):
    off = 12 + i * 16
    tag = d[off:off+4].decode("latin-1")
    toff, tlen = struct.unpack(">II", d[off+8:off+16])
    tables[tag] = (toff, tlen)

if "name" in tables:
    noff = tables["name"][0]
    fmt, count, strOff = struct.unpack(">HHH", d[noff:noff+6])
    for i in range(count):
        r = noff + 6 + i * 12
        pid, eid, lid, nid, ln, off = struct.unpack(">HHHHHH", d[r:r+12])
        if nid == 4:
            raw = d[noff+strOff+off: noff+strOff+off+ln]
            try:
                print("face:", raw.decode("utf-16-be" if pid == 3 else "latin-1"))
            except Exception:
                pass
            break

coff = tables["cmap"][0]
ntab = struct.unpack(">H", d[coff+2:coff+4])[0]
best = None
for i in range(ntab):
    r = coff + 4 + i * 8
    pid, eid, off = struct.unpack(">HHI", d[r:r+8])
    fmt = struct.unpack(">H", d[coff+off:coff+off+2])[0]
    if fmt == 4:
        best = coff + off
        break
if best is None:
    sys.exit("no format-4 cmap")

segX2 = struct.unpack(">H", d[best+6:best+8])[0]
seg = segX2 // 2
ends   = struct.unpack(">%dH" % seg, d[best+14:best+14+segX2])
starts = struct.unpack(">%dH" % seg, d[best+16+segX2:best+16+2*segX2])
deltas = struct.unpack(">%dh" % seg, d[best+16+2*segX2:best+16+3*segX2])
rangeOffBase = best + 16 + 3*segX2
rangeOffs = struct.unpack(">%dH" % seg, d[rangeOffBase:rangeOffBase+segX2])

def gid(cp):
    for i in range(seg):
        if cp <= ends[i] and cp >= starts[i]:
            if rangeOffs[i] == 0:
                return (cp + deltas[i]) & 0xFFFF
            addr = rangeOffBase + i*2 + rangeOffs[i] + (cp - starts[i]) * 2
            if addr + 2 > len(d):
                return 0
            g = struct.unpack(">H", d[addr:addr+2])[0]
            return 0 if g == 0 else (g + deltas[i]) & 0xFFFF
    return 0

def report(a, b, label):
    n = sum(1 for cp in range(a, b+1) if gid(cp))
    print("  %-24s %3d / %d" % (label, n, b-a+1))

report(0x20, 0x7E, "ASCII")
report(0xA0, 0xFF, "Latin-1 supplement")
report(0x400, 0x45F, "Cyrillic")
missing = [cp for cp in range(0x410, 0x450) if not gid(cp)]
print("  missing Russian letters:", " ".join("U+%04X" % c for c in missing) or "(none)")
