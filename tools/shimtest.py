#!/usr/bin/env python3
"""Put the zlib and iconv shims beside the real thing and compare bytes.

Both shims are built for the host here -- the same sources the cross build
uses -- and run against files whose right answer is known, because gzip and
iconv produced it. Nothing is asserted from memory.
"""
import os, subprocess, sys, gzip, random

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
PU = XYUOS_SRC + "/ns/libparserutils-0.2.4"
OUT = "/tmp/shimcheck"
os.makedirs(OUT, exist_ok=True)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


# --- libparserutils, built for the host so the iconv shim has its codecs ---
pu_objs = []
for root, _, files in os.walk(PU + "/src"):
    for f in sorted(files):
        if not f.endswith(".c"):
            continue
        src = os.path.join(root, f)
        obj = os.path.join(OUT, os.path.relpath(src, PU + "/src")
                                  .replace("/", "_")[:-2] + ".o")
        r = run(["gcc", "-O1", "-w", "-fcommon", "-std=gnu99",
                 "-I" + PU + "/include", "-I" + PU + "/src",
                 "-c", src, "-o", obj])
        if r.returncode != 0:
            sys.exit("libparserutils %s did not build for the host:\n%s"
                     % (f, r.stderr.decode()))
        pu_objs.append(obj)

lib = OUT + "/libparserutils.a"
if os.path.exists(lib):
    os.remove(lib)
run(["ar", "rcs", lib] + pu_objs)
print("libparserutils: %d objects built for the host" % len(pu_objs))

# --- the probe, against the shims ------------------------------------------
r = run(["gcc", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-o", OUT + "/probe",
         "-I" + HOME + "/third_party/shim/include",
         "-I" + HOME + "/userland",
         "-I" + PU + "/include",
         HOME + "/tools/shim_probe.c",
         HOME + "/third_party/shim/src/zlib_shim.c",
         HOME + "/third_party/shim/src/iconv_shim.c",
         lib])
if r.returncode != 0:
    sys.exit("the probe did not build:\n" + r.stderr.decode())
if r.stderr.strip():
    print("warnings:\n" + r.stderr.decode().strip())

PROBE = OUT + "/probe"
fails = []


def check(name, got, want):
    if got == want:
        print("  ok    %s" % name)
    else:
        print("  FAIL  %s (%d bytes back, %d expected)"
              % (name, len(got), len(want)))
        for i in range(min(len(got), len(want))):
            if got[i] != want[i]:
                print("        first difference at byte %d: %r vs %r"
                      % (i, got[i:i + 16], want[i:i + 16]))
                break
        fails.append(name)


# --- gzip ------------------------------------------------------------------
print("\ngzip:")

random.seed(7)
bodies = {
    "lines": "\n".join("key%d:value number %d" % (i, i)
                       for i in range(500)).encode() + b"\n",
    "one line, no newline at the end": b"solitary",
    "empty": b"",
    "long line": b"x" * 100000 + b"\n",
    # Random bytes do not compress, so this exercises the stored-block path
    # that a text file never reaches.
    "incompressible": bytes(random.randrange(256) for _ in range(20000)),
    "repetitive": (b"abcabcabc" * 5000),
}

for name, body in bodies.items():
    raw = OUT + "/body.bin"
    gz = OUT + "/body.gz"
    open(raw, "wb").write(body)
    open(gz, "wb").write(gzip.compress(body))

    got = run([PROBE, "zs", gz])
    if got.returncode != 0 and body == b"":
        print("  ok    z_stream, %s (refused an empty stream, as zlib does)"
              % name)
    else:
        check("z_stream, %s" % name, got.stdout, body)

    # gzgets returns a C string, so it cannot carry a NUL byte. Binary
    # content goes through gzread instead; that is zlib's own division of
    # labour, not a shortcoming of this one.
    mode = "zr" if b"\0" in body else "gz"
    what = "gzread" if mode == "zr" else "gzgets"

    got = run([PROBE, mode, gz])
    check("%s, %s" % (what, name), got.stdout, body)

    # An uncompressed file must read through gzopen unchanged, which is what
    # the message catalogue relies on when it was never compressed.
    got = run([PROBE, mode, raw])
    check("%s on plain bytes, %s" % (what, name), got.stdout, body)

# --- iconv -----------------------------------------------------------------
print("\niconv:")

TEXTS = {
    "russian":  "Здравствуй, мир! Съешь ещё этих мягких булок.",
    "european": "Grüße, naïve café — jalapeño Ærø",
    "ascii":    "plain 7-bit text, 0123456789",
    "long":     ("Привет " * 3000),
}

# KOI8-R is deliberately absent: libparserutils carries the nine Windows
# codepages, the ISO 8859 parts, ASCII and the Unicode forms, and no KOI8.
# An iconv built on those codecs supports what they support, and refusing an
# encoding it cannot do is the right answer -- see the note at the end.
PAIRS = [
    ("UTF-8", "WINDOWS-1251", "russian"),
    ("WINDOWS-1251", "UTF-8", "russian"),
    ("UTF-8", "WINDOWS-1252", "european"),
    ("WINDOWS-1252", "UTF-8", "european"),
    ("UTF-8", "ISO-8859-1", "european"),
    ("UTF-8", "US-ASCII", "ascii"),
    ("UTF-8", "UTF-8", "russian"),
    ("UTF-8", "WINDOWS-1251", "long"),
]

for frm, to, which in PAIRS:
    text = TEXTS[which]
    try:
        src = text.encode(frm.replace("US-ASCII", "ascii"))
    except UnicodeEncodeError:
        continue
    try:
        want = text.encode(to.replace("US-ASCII", "ascii"))
    except UnicodeEncodeError:
        continue

    path = OUT + "/text.bin"
    open(path, "wb").write(src)
    got = run([PROBE, "iconv", frm, to, path])
    if got.returncode != 0:
        print("  FAIL  %s -> %s (%s): %s"
              % (frm, to, which, got.stderr.decode().strip()))
        fails.append("%s -> %s" % (frm, to))
        continue
    check("%s -> %s (%s)" % (frm, to, which), got.stdout, want)

# A character the destination cannot hold must be reported, not swallowed:
# NetSurf answers that report by writing the character as an HTML entity.
path = OUT + "/text.bin"
open(path, "wb").write("price: 100€".encode("utf-8"))
got = run([PROBE, "iconv", "UTF-8", "ISO-8859-1", path])
if got.returncode == 3:
    print("  ok    an unrepresentable character is reported (%s)"
          % got.stderr.decode().strip().split(": ")[-1].split(" at ")[0])
else:
    print("  FAIL  an unrepresentable character was not reported (rc=%d)"
          % got.returncode)
    fails.append("unrepresentable character")

# An encoding the codecs do not carry must be refused at iconv_open, not
# accepted and then got wrong.
open(path, "wb").write("Привет".encode("koi8-r"))
got = run([PROBE, "iconv", "KOI8-R", "UTF-8", path])
if got.returncode == 2 and b"iconv_open" in got.stderr:
    print("  ok    an encoding these codecs do not carry is refused (KOI8-R)")
else:
    print("  FAIL  KOI8-R was not refused cleanly (rc=%d)" % got.returncode)
    fails.append("KOI8-R refusal")

# Broken UTF-8 must be reported too, rather than silently replaced.
open(path, "wb").write(b"good \xc3\x28 bad")
got = run([PROBE, "iconv", "UTF-8", "WINDOWS-1251", path])
if got.returncode == 3:
    print("  ok    an invalid byte sequence is reported")
else:
    print("  FAIL  an invalid byte sequence was not reported (rc=%d)"
          % got.returncode)
    fails.append("invalid sequence")

print()
if fails:
    sys.exit("%d check(s) failed: %s" % (len(fails), ", ".join(fails)))
print("every check passed")
