#!/usr/bin/env python3
"""Compile NetSurf's libraries with the xyuOS cross toolchain and report the gap.

A port is not decided by reading about it. This takes each library's source
files one at a time, builds them against xyuOS's own libc with the same flags
its own userland is built with, and writes down exactly what fails and why --
so the work that remains is a list rather than a feeling.
"""
import os, re, subprocess, sys, collections

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
NS = "/tmp/ns"
CC = HOME + "/toolchain/cross/bin/x86_64-elf-gcc"
OUT = "/tmp/ns/o"

FLAGS = ["-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
         "-mno-red-zone", "-msse", "-msse2", "-std=gnu11", "-O2", "-w",
         "-I" + HOME + "/libc/include"]

LIBS = ["libwapcaplet-0.4.3", "libparserutils-0.2.4", "libhubbub-0.3.8",
        "libcss-0.9.1", "libdom-0.4.2"]

os.makedirs(OUT, exist_ok=True)

# Every library's headers are on the path for every other: that is what
# installing them would have done.
incs = []
for l in LIBS:
    for sub in ("include", "src"):
        d = "%s/%s/%s" % (NS, l, sub)
        if os.path.isdir(d):
            incs.append("-I" + d)

missing = collections.Counter()      # header -> how many files wanted it
errors = collections.Counter()       # first error line -> count
per_lib = {}

for lib in LIBS:
    root = "%s/%s" % (NS, lib)
    if not os.path.isdir(root):
        continue
    srcs = []
    for base, _, files in os.walk(root + "/src"):
        for f in files:
            if f.endswith(".c"):
                srcs.append(os.path.join(base, f))
    ok = bad = 0
    for c in sorted(srcs):
        r = subprocess.run([CC] + FLAGS + incs + ["-I" + root, "-c", c,
                                                  "-o", OUT + "/t.o"],
                           capture_output=True, text=True)
        if r.returncode == 0:
            ok += 1
            continue
        bad += 1
        err = r.stderr
        m = re.search(r"fatal error: ([^:]+): No such file", err)
        if m:
            missing[m.group(1).strip()] += 1
            continue
        # The first real complaint, with the file and line stripped off so
        # that the same problem in twenty files counts once.
        for line in err.splitlines():
            if " error: " in line:
                errors[line.split(" error: ", 1)[1].strip()[:90]] += 1
                break
        else:
            errors["(no error line: %s)" % err.strip().splitlines()[:1]] += 1
    per_lib[lib] = (ok, bad, len(srcs))

print("Built against xyuOS's libc, with xyuOS's own userland flags:\n")
tot_ok = tot_bad = 0
for lib in LIBS:
    if lib not in per_lib:
        continue
    ok, bad, n = per_lib[lib]
    tot_ok += ok
    tot_bad += bad
    print("   %-22s %3d of %3d built, %3d failed" % (lib.split("-")[0], ok, n, bad))
print("\n   %-22s %3d built, %3d failed" % ("in total", tot_ok, tot_bad))

if missing:
    print("\nHeaders the libraries want and xyuOS has not got:")
    for h, n in missing.most_common(30):
        print("   %-28s wanted by %d file(s)" % (h, n))

if errors:
    print("\nEverything else that went wrong:")
    for e, n in errors.most_common(20):
        print("   %3d x  %s" % (n, e))
