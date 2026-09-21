#!/usr/bin/env python3
"""Build the xyuOS side of the NetSurf port: the fetcher, and later the
frontend. These are xyuOS's own files, not NetSurf's, and they live outside
its tree so that nothing here is a patch to somebody else's source."""
import os, subprocess, sys

HOME = "/home/roman/xyuos-neo"
NS = "/home/roman/src/ns/netsurf-3.9"
CC = HOME + "/toolchain/cross/bin/x86_64-elf-gcc"
AR = HOME + "/toolchain/cross/bin/x86_64-elf-ar"
GLUE = HOME + "/third_party/nsxyuos"
OUT = "/home/roman/src/ns/glue-obj"

FLAGS = ["-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
         "-mno-red-zone", "-msse", "-msse2", "-std=gnu11", "-O2",
         "-Wall", "-Wextra", "-Wno-unused-parameter", "-fcommon",
         "-D_ALIGNED=", "-Dnsxyuos",
         "-DWITH_CSS", "-DWITH_HTML", "-DWITH_TEXT",
         "-DNETSURF_UA_FORMAT_STRING=\"%s\"",
         "-DNETSURF_HOMEPAGE=\"about:welcome\"",
         "-DNETSURF_BUILTIN_LOG_FILTER=\"level:WARNING\"",
         "-DNETSURF_BUILTIN_VERBOSE_FILTER=\"level:DEBUG\""]

INC = ["-I" + HOME + "/libc/include",
       "-I" + HOME + "/third_party/netsurf/include",
       "-I" + HOME + "/third_party/shim/include",
       "-I" + HOME + "/userland",          # inflate.h
       "-I" + GLUE,
       "-I" + NS, "-I" + NS + "/include",
       "-I" + NS + "/content/handlers",
       "-I" + NS + "/frontends",
       "-I/home/roman/src/ns"]                        # testament.h

os.makedirs(OUT, exist_ok=True)

srcs = sorted(f for f in os.listdir(GLUE) if f.endswith(".c"))
if not srcs:
    sys.exit("nothing to build in " + GLUE)

objs, bad = [], 0
for f in srcs:
    obj = os.path.join(OUT, f[:-2] + ".o")
    r = subprocess.run([CC] + FLAGS + INC + ["-c", GLUE + "/" + f, "-o", obj],
                       capture_output=True, text=True)
    if r.returncode != 0:
        bad += 1
        print("FAILED  " + f)
        print("\n".join(r.stderr.strip().splitlines()[:30]))
    else:
        if r.stderr.strip():
            print("warnings in " + f + ":")
            print("\n".join(r.stderr.strip().splitlines()[:30]))
        objs.append(obj)
        print("built   " + f)

if bad:
    sys.exit("\n%d file(s) did not build" % bad)

lib = HOME + "/third_party/nsxyuos/libnsglue.a"
if os.path.exists(lib):
    os.remove(lib)
subprocess.run([AR, "rcs", lib] + objs, check=True)

nm = subprocess.run([HOME + "/toolchain/cross/bin/x86_64-elf-nm", "-g",
                     "--defined-only", lib], capture_output=True, text=True)
names = sorted({ln.split()[-1] for ln in nm.stdout.splitlines() if " T " in ln})
print("\n%s\n  provides: %s" % (lib, " ".join(names)))

und = sorted({ln.split()[-1] for ln in
              subprocess.run([HOME + "/toolchain/cross/bin/x86_64-elf-nm",
                              "-u", lib], capture_output=True,
                             text=True).stdout.splitlines() if ln.strip()})
print("  wants:    %s" % " ".join(und))
