#!/usr/bin/env python3
"""Make small WebP files with known contents and check our decoder on them.

A photograph that comes out wrong tells you nothing about where. These are a
few pixels each, chosen to turn one feature of the format on at a time -- two
colours to force the palette transform, seventeen to turn it off again, a
gradient to exercise the predictors, noise to defeat them -- so that a failure
names its own cause.
"""

import ctypes
import ctypes.util
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = "/tmp/webpcheck"


def load():
    for cand in [ctypes.util.find_library("webp"), "libwebp.so.7", "libwebp.so"]:
        if not cand:
            continue
        try:
            return ctypes.CDLL(cand)
        except OSError:
            pass
    sys.exit("needs libwebp")


L = load()


def encode_lossless(rgba, w, h):
    out = ctypes.POINTER(ctypes.c_ubyte)()
    L.WebPEncodeLosslessRGBA.restype = ctypes.c_size_t
    n = L.WebPEncodeLosslessRGBA(bytes(rgba), w, h, w * 4, ctypes.byref(out))
    if not n:
        return None
    data = bytes(bytearray(out[:n]))
    L.WebPFree(out)
    return data


def encode_lossy(rgb, w, h, q):
    out = ctypes.POINTER(ctypes.c_ubyte)()
    L.WebPEncodeRGB.restype = ctypes.c_size_t
    n = L.WebPEncodeRGB(bytes(rgb), w, h, w * 3, ctypes.c_float(q),
                        ctypes.byref(out))
    if not n:
        return None
    data = bytes(bytearray(out[:n]))
    L.WebPFree(out)
    return data


def decode_ref(data):
    w, h = ctypes.c_int(), ctypes.c_int()
    L.WebPDecodeRGBA.restype = ctypes.POINTER(ctypes.c_ubyte)
    p = L.WebPDecodeRGBA(data, ctypes.c_size_t(len(data)),
                         ctypes.byref(w), ctypes.byref(h))
    if not p:
        return None
    n = w.value * h.value * 4
    raw = bytes(bytearray(p[:n]))
    L.WebPFree(p)
    out = bytearray(w.value * h.value * 3)
    for i in range(w.value * h.value):
        r, g, b, a = raw[i*4], raw[i*4+1], raw[i*4+2], raw[i*4+3]
        if a != 255:
            r = (r * a + 255 * (255 - a)) // 255
            g = (g * a + 255 * (255 - a)) // 255
            b = (b * a + 255 * (255 - a)) // 255
        out[i*3], out[i*3+1], out[i*3+2] = r, g, b
    return w.value, h.value, bytes(out)


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    fields, i = [], 2
    while len(fields) < 3:
        while d[i:i+1].isspace():
            i += 1
        j = i
        while not d[j:j+1].isspace():
            j += 1
        fields.append(int(d[i:j]))
        i = j
    i += 1
    w, h, _ = fields
    return w, h, d[i:i + w * h * 3]


def patterns(w, h):
    random.seed(7)
    out = {}

    def mk(f):
        px = bytearray()
        for y in range(h):
            for x in range(w):
                px += bytes(f(x, y))
        return px

    out["solid"]       = mk(lambda x, y: (200, 100, 50, 255))
    out["two_colour"]  = mk(lambda x, y: (0, 0, 0, 255) if (x + y) & 1
                            else (255, 255, 255, 255))
    out["four_colour"] = mk(lambda x, y: [(0, 0, 0, 255), (255, 0, 0, 255),
                                          (0, 255, 0, 255), (0, 0, 255, 255)]
                            [(x // 2 + y) & 3])
    out["sixteen"]     = mk(lambda x, y: (17 * ((x + y) % 16), 0, 0, 255))
    out["seventeen"]   = mk(lambda x, y: (15 * ((x * 3 + y) % 17), 40, 90, 255))
    out["gradient"]    = mk(lambda x, y: ((x * 255) // max(w - 1, 1),
                                          (y * 255) // max(h - 1, 1), 128, 255))
    out["noise"]       = mk(lambda x, y: (random.randrange(256),
                                          random.randrange(256),
                                          random.randrange(256), 255))
    out["alpha"]       = mk(lambda x, y: (200, 30, 30,
                                          (x * 255) // max(w - 1, 1)))
    return out


def check(exe, name, data, w, h):
    path = os.path.join(WORK, "small_%s.webp" % name)
    with open(path, "wb") as f:
        f.write(data)
    ref = decode_ref(data)
    if ref is None:
        print("%-30s libwebp cannot decode its own output" % name)
        return False
    out = path + ".ours.ppm"
    r = subprocess.run([exe, path, out], capture_output=True, text=True)
    if r.returncode:
        print("%-30s FAILED: %s" % (name, r.stderr.strip()))
        return False
    got = read_ppm(out)
    if (got[0], got[1]) != (ref[0], ref[1]):
        print("%-30s size %dx%d, expected %dx%d"
              % (name, got[0], got[1], ref[0], ref[1]))
        return False
    g, rr = got[2], ref[2]
    nbad = sum(1 for i in range(len(rr)) if g[i] != rr[i])
    for i in range(len(rr)):
        if g[i] != rr[i]:
            px = i // 3
            print("%-30s %d bytes wrong; first (%d,%d) ch%d: %d vs %d"
                  % (name, nbad, px % w, px // w, i % 3, g[i], rr[i]))
            return False
    print("%-30s exact" % name)
    return True


def main():
    os.makedirs(WORK, exist_ok=True)
    exe = os.path.join(WORK, "webpcheck")
    subprocess.run(["cc", "-O2", "-w", "-o", exe,
                    os.path.join(HERE, "webpcheck.c"), "-lm"], check=True)

    ok = bad = 0
    for (w, h) in ((16, 16), (37, 11), (64, 48), (200, 150), (400, 301)):
        print("--- %dx%d" % (w, h))
        pats = patterns(w, h)
        for name, rgba in pats.items():
            d = encode_lossless(rgba, w, h)
            if d and check(exe, "ll_%s_%dx%d" % (name, w, h), d, w, h):
                ok += 1
            else:
                bad += 1
        rgb = bytearray()
        grad = pats["gradient"]
        for i in range(0, len(grad), 4):
            rgb += grad[i:i + 3]
        for q in (50, 90):
            d = encode_lossy(rgb, w, h, q)
            if d and check(exe, "lossy_q%d_%dx%d" % (q, w, h), d, w, h):
                ok += 1
            else:
                bad += 1
    print("\n%d exact, %d wrong" % (ok, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
