#!/usr/bin/env python3
"""Check our WebP decoder against libwebp, pixel for pixel.

WebP is defined on integers: given the same file, every correct decoder
produces exactly the same bytes. So this does not measure similarity, it
looks for any difference at all -- the same method tools/runmathcheck.sh uses
against glibc, and for the same reason. A decoder that is nearly right is a
decoder with a bug somebody has not tripped over yet.

    python3 tools/webpcheck.py                 # fetches a set of test images
    python3 tools/webpcheck.py a.webp b.webp   # checks the named files

Needs libwebp installed for the reference (libwebp7 on Debian and Ubuntu).
"""

import ctypes
import ctypes.util
import os
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = "/tmp/webpcheck"

# A spread of the things the format can be: lossy photographs, lossless
# graphics, an image with an alpha plane, and a small one to exercise the
# partial macroblocks at the right and bottom edges.
SAMPLES = [
    ("lossy_photo.webp",
     "https://www.gstatic.com/webp/gallery/1.webp"),
    ("lossy_photo2.webp",
     "https://www.gstatic.com/webp/gallery/4.webp"),
    ("lossless_graphic.webp",
     "https://www.gstatic.com/webp/gallery3/1_webp_ll.webp"),
    ("lossless_alpha.webp",
     "https://www.gstatic.com/webp/gallery3/2_webp_ll.webp"),
    ("lossy_alpha.webp",
     "https://www.gstatic.com/webp/gallery3/5_webp_a.webp"),
]


def load_libwebp():
    name = ctypes.util.find_library("webp")
    for cand in ([name] if name else []) + ["libwebp.so.7", "libwebp.so"]:
        try:
            return ctypes.CDLL(cand)
        except OSError:
            continue
    sys.exit("webpcheck: libwebp is not installed (apt install libwebp7)")


def reference(lib, data):
    """Decode with libwebp; returns (w, h, rgb bytes) flattened onto white."""
    w = ctypes.c_int()
    h = ctypes.c_int()
    lib.WebPDecodeRGBA.restype = ctypes.POINTER(ctypes.c_ubyte)
    p = lib.WebPDecodeRGBA(data, ctypes.c_size_t(len(data)),
                           ctypes.byref(w), ctypes.byref(h))
    if not p:
        return None
    n = w.value * h.value * 4
    raw = bytes(bytearray(p[:n]))
    lib.WebPFree(p)

    out = bytearray(w.value * h.value * 3)
    for i in range(w.value * h.value):
        r, g, b, a = raw[i*4], raw[i*4+1], raw[i*4+2], raw[i*4+3]
        if a != 255:
            # The same flattening onto white that img.h does, since our
            # surface carries no alpha channel.
            r = (r * a + 255 * (255 - a)) // 255
            g = (g * a + 255 * (255 - a)) // 255
            b = (b * a + 255 * (255 - a)) // 255
        out[i*3], out[i*3+1], out[i*3+2] = r, g, b
    return w.value, h.value, bytes(out)


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return None
    # header: P6 \n w h \n 255 \n
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":
            while data[i:i+1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = fields
    return w, h, data[i:i + w * h * 3]


def build(extra=()):
    exe = os.path.join(WORK, "webpcheck" + ("_nf" if extra else ""))
    cmd = (["cc", "-O2", "-w", "-o", exe] + list(extra) +
           [os.path.join(HERE, "webpcheck.c"), "-lm"])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("webpcheck: our decoder does not compile for the host")
    return exe


def decode(exe, path, tag):
    out = os.path.join(WORK, os.path.basename(path) + "." + tag + ".ppm")
    r = subprocess.run([exe, path, out], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr.strip()
    return read_ppm(out), None


def main():
    os.makedirs(WORK, exist_ok=True)
    lib = load_libwebp()
    exe = build()
    exe_nf = build(("-DVP8_NO_LOOP_FILTER",))

    files = sys.argv[1:]
    if not files:
        files = []
        for name, url in SAMPLES:
            path = os.path.join(WORK, name)
            if not os.path.exists(path):
                try:
                    print("fetching %s ..." % name)
                    urllib.request.urlretrieve(url, path)
                except Exception as e:
                    print("  could not fetch %s (%s)" % (name, e))
                    continue
            files.append(path)

    if not files:
        sys.exit("webpcheck: nothing to check")

    worst = 0
    failures = 0
    for path in files:
        name = os.path.basename(path)
        with open(path, "rb") as f:
            data = f.read()

        ref = reference(lib, data)
        if ref is None:
            print("%-24s libwebp will not decode it either -- skipped" % name)
            continue
        rw, rh, rpx = ref

        got, err = decode(exe, path, "ours")
        if got is None:
            print("%-24s OURS FAILED: %s" % (name, err))
            failures += 1
            continue
        gw, gh, gpx = got

        # The same image decoded without the deblocking filter. Comparing both
        # against the reference says whether a difference came from the filter
        # or from what it was given.
        nf, _ = decode(exe_nf, path, "nofilter")

        if (gw, gh) != (rw, rh):
            print("%-24s SIZE %dx%d, libwebp says %dx%d" % (name, gw, gh, rw, rh))
            failures += 1
            continue

        diff = 0
        maxd = 0
        firsts = []
        # Where the differences fall says what is wrong. Errors only on the
        # seams between 4x4 blocks point at the loop filter; errors spread
        # evenly through a block point at prediction or the transform.
        on_edge = 0
        for i in range(len(rpx)):
            d = gpx[i] - rpx[i]
            if d:
                diff += 1
                d = abs(d)
                if d > maxd:
                    maxd = d
                px = i // 3
                x, y = px % gw, px // gw
                if len(firsts) < 6:
                    firsts.append((x, y, gpx[i], rpx[i]))
                if x % 4 in (0, 3) or y % 4 in (0, 3):
                    on_edge += 1
        total = len(rpx)
        if diff == 0:
            print("%-24s %4dx%-4d  exact" % (name, gw, gh))
        else:
            pct = 100.0 * diff / total
            print("%-24s %4dx%-4d  %d of %d bytes differ (%.3f%%), worst by %d"
                  % (name, gw, gh, diff, total, pct, maxd))
            if nf is not None and (nf[0], nf[1]) == (gw, gh):
                npx = nf[2]
                # Pixels the filter never touched: identical in both our
                # builds. If those already differ from libwebp, the error is
                # in the reconstruction and not in the filter.
                untouched = 0
                untouched_bad = 0
                for i in range(len(rpx)):
                    if gpx[i] == npx[i]:
                        untouched += 1
                        if gpx[i] != rpx[i]:
                            untouched_bad += 1
                print("%-24s   the filter left %d bytes alone; %d of those are "
                      "already wrong" % ("", untouched, untouched_bad))
            print("%-24s   first: %s" % ("", ", ".join(
                "(%d,%d) %d vs %d" % f for f in firsts)))
            failures += 1
            worst = max(worst, maxd)

    print()
    if failures:
        print("%d of %d did not match" % (failures, len(files)))
        return 1
    print("all %d match libwebp exactly" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
