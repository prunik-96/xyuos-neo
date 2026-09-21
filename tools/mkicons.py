#!/usr/bin/env python3
"""Turn assets/icons/*.png into one blob the OS can use directly.

The kernel draws the taskbar and the start menu, and the kernel has no PNG
decoder -- nor should it grow one just to put a picture on a button. So the
decoding, the background removal and the scaling all happen here, at build
time, and what ships is raw ARGB at exactly the two sizes the interface uses.

Background removal is a flood fill from the border, not a colour key. The
icons are drawn on white, and several of them are white cards with dark
outlines: keying every white pixel would punch holes straight through the
documents. Only white that is CONNECTED TO THE EDGE is background.

If an icon already carries an alpha channel this leaves it alone -- so the day
the drawings arrive with real transparency, nothing here has to change.
"""
import os, struct, subprocess, sys, zlib

SRC = sys.argv[1] if len(sys.argv) > 1 else "assets/icons"
OUT = sys.argv[2] if len(sys.argv) > 2 else "assets/icons.bin"

BIG, SMALL = 32, 16
NEAR_WHITE = 244          # a pixel this bright in every channel counts as paper


def load_rgba(path, size):
    """Decode and scale to size x size, as a flat RGBA bytearray."""
    r = subprocess.run(
        ["convert", path, "-alpha", "on", "-resize", "%dx%d!" % (size, size),
         "-depth", "8", "RGBA:-"],
        capture_output=True)
    if r.returncode != 0 or len(r.stdout) != size * size * 4:
        raise SystemExit("cannot decode %s (%s)" % (path, r.stderr[:200]))
    return bytearray(r.stdout)


def has_alpha(px):
    return any(px[i] != 255 for i in range(3, len(px), 4))


def strip_background(px, size):
    """Flood fill near-white inward from the border and make it transparent."""
    def white(i):
        return px[i] >= NEAR_WHITE and px[i+1] >= NEAR_WHITE and px[i+2] >= NEAR_WHITE

    seen = bytearray(size * size)
    stack = []
    for x in range(size):
        stack.append((x, 0)); stack.append((x, size - 1))
    for y in range(size):
        stack.append((0, y)); stack.append((size - 1, y))

    while stack:
        x, y = stack.pop()
        if x < 0 or y < 0 or x >= size or y >= size:
            continue
        n = y * size + x
        if seen[n]:
            continue
        if not white(n * 4):
            continue
        seen[n] = 1
        px[n * 4 + 3] = 0
        stack.append((x + 1, y)); stack.append((x - 1, y))
        stack.append((x, y + 1)); stack.append((x, y - 1))

    # Soften what is left of the antialiased rim: a pixel next to the cleared
    # area that is still pale is half paper, and drawn opaque it shows up as a
    # bright fringe against a dark desktop.
    for y in range(size):
        for x in range(size):
            n = y * size + x
            if px[n * 4 + 3] == 0:
                continue
            near_cleared = False
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ax, ay = x + dx, y + dy
                if 0 <= ax < size and 0 <= ay < size and seen[ay * size + ax]:
                    near_cleared = True
                    break
            if not near_cleared:
                continue
            r, g, b = px[n*4], px[n*4+1], px[n*4+2]
            lum = (r * 30 + g * 59 + b * 11) // 100
            if lum > 200:                      # nearly paper: fade it out
                px[n * 4 + 3] = int(255 * (255 - lum) / 55)
    return px


def box_down(px, src, dst):
    """Average-filter src x src RGBA down to dst x dst, alpha-weighted."""
    out = bytearray(dst * dst * 4)
    step = src / dst
    for y in range(dst):
        y0, y1 = int(y * step), max(int((y + 1) * step), int(y * step) + 1)
        for x in range(dst):
            x0, x1 = int(x * step), max(int((x + 1) * step), int(x * step) + 1)
            ar = ag = ab = aa = n = 0
            for sy in range(y0, min(y1, src)):
                for sx in range(x0, min(x1, src)):
                    i = (sy * src + sx) * 4
                    a = px[i + 3]
                    # Weight colour by coverage, or transparent pixels drag the
                    # edges towards whatever colour happens to sit behind them.
                    ar += px[i] * a; ag += px[i+1] * a; ab += px[i+2] * a
                    aa += a; n += 1
            o = (y * dst + x) * 4
            if aa:
                out[o] = ar // aa; out[o+1] = ag // aa; out[o+2] = ab // aa
            out[o + 3] = aa // max(n, 1)
    return out


def to_argb(px):
    """RGBA bytes -> little-endian 0xAARRGGBB words, what the compositor wants."""
    out = bytearray(len(px))
    for i in range(0, len(px), 4):
        r, g, b, a = px[i], px[i+1], px[i+2], px[i+3]
        out[i]   = b
        out[i+1] = g
        out[i+2] = r
        out[i+3] = a
    return out


def main():
    names = sorted(f[:-4] for f in os.listdir(SRC) if f.endswith(".png"))
    if not names:
        raise SystemExit("no icons in " + SRC)

    index, blob = bytearray(), bytearray()
    kept = []
    for name in names:
        if len(name) >= 24:
            print("  skipping %s: name too long" % name)
            continue
        px = load_rgba(os.path.join(SRC, name + ".png"), BIG)
        if not has_alpha(px):
            px = strip_background(px, BIG)
        small = box_down(px, BIG, SMALL)

        off_big = len(blob)
        blob += to_argb(px)
        off_small = len(blob)
        blob += to_argb(small)

        index += name.encode() + b"\0" * (24 - len(name))
        index += struct.pack("<II", off_big, off_small)
        kept.append(name)

    header = b"XICO" + struct.pack("<III", len(kept), BIG, SMALL)
    with open(OUT, "wb") as f:
        f.write(header + index + blob)
    print("icons: %d packed into %s (%d KB)" %
          (len(kept), OUT, (len(header) + len(index) + len(blob)) // 1024))


main()
