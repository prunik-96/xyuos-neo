#!/usr/bin/env python3
"""Build the shims NetSurf's core wants and this system did not have.

Two headers, two files. Neither invents behaviour: zlib.h is a shape put
around the DEFLATE the picture decoder already contained, and iconv.h is
libparserutils' own codecs joined back to back through UCS-4.
"""
import os, subprocess, sys

HOME = "/home/roman/xyuos-neo"
CC = HOME + "/toolchain/cross/bin/x86_64-elf-gcc"
AR = HOME + "/toolchain/cross/bin/x86_64-elf-ar"
SHIM = HOME + "/third_party/shim"
OUT = "/home/roman/src/ns/shim-obj"

FLAGS = ["-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
         "-mno-red-zone", "-msse", "-msse2", "-std=gnu11", "-O2",
         "-Wall", "-Wextra", "-Wno-unused-parameter"]

INC = ["-I" + HOME + "/libc/include",
       "-I" + SHIM + "/include",
       "-I" + HOME + "/userland",              # inflate.h lives with the
       "-I" + HOME + "/third_party/netsurf/include"]

os.makedirs(OUT, exist_ok=True)

srcs = sorted(f for f in os.listdir(SHIM + "/src") if f.endswith(".c"))
objs, bad = [], 0

for f in srcs:
    obj = os.path.join(OUT, f[:-2] + ".o")
    r = subprocess.run([CC] + FLAGS + INC + ["-c", SHIM + "/src/" + f,
                                             "-o", obj],
                       capture_output=True, text=True)
    if r.returncode != 0:
        bad += 1
        print("FAILED  " + f)
        print("\n".join(r.stderr.strip().splitlines()[:25]))
    else:
        if r.stderr.strip():
            print("warnings in " + f + ":")
            print("\n".join(r.stderr.strip().splitlines()[:25]))
        objs.append(obj)
        print("built   " + f)

if bad:
    sys.exit("\n%d file(s) did not build; no archive written" % bad)

lib = HOME + "/third_party/shim/libnsshim.a"
if os.path.exists(lib):
    os.remove(lib)
subprocess.run([AR, "rcs", lib] + objs, check=True)

n = subprocess.run([HOME + "/toolchain/cross/bin/x86_64-elf-nm", "-g",
                    "--defined-only", lib],
                   capture_output=True, text=True).stdout
names = sorted({ln.split()[-1] for ln in n.splitlines() if " T " in ln})
print("\n%s" % lib)
print("  provides: " + " ".join(names))
