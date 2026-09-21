#!/usr/bin/env python3
"""Build NetSurf's JavaScript: Duktape, and the DOM bindings around it.

The engine is vendored in NetSurf's tree, but what makes it a browser rather
than a calculator is the bindings -- the code that lets a script reach
document, window, an element's style. Those are GENERATED, by nsgenbind, from
WebIDL plus NetSurf's own .bnd files. Two hundred and thirty-nine files of
them.

That generation is done once by tools/nsprep_js.py; this compiles the result.
Nothing here is written by hand, which is the point: the bindings are a
mechanical consequence of the specification, and hand-editing them would be
lost the next time they were generated.
"""
import os, re, subprocess, sys, collections

HOME = "/home/roman/xyuos-neo"
NS = "/home/roman/src/ns/netsurf-3.9"
DUK = "/home/roman/src/ns/duk"
CC = HOME + "/toolchain/cross/bin/x86_64-elf-gcc"
AR = HOME + "/toolchain/cross/bin/x86_64-elf-ar"
OUT = "/home/roman/src/ns/js-obj"

if not os.path.exists(DUK + "/binding.h"):
    sys.exit("no bindings -- run tools/nsprep_js.py first")

FLAGS = ["-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
         "-mno-red-zone", "-msse", "-msse2", "-std=gnu11", "-O2", "-w",
         "-fcommon", "-D_ALIGNED=", "-D_POSIX_C_SOURCE=200809L",
         "-DWITH_CSS", "-DWITH_HTML", "-DWITH_TEXT", "-Dnsxyuos",
         "-DNETSURF_UA_FORMAT_STRING=\"%s\"",
         "-DNETSURF_HOMEPAGE=\"about:welcome\"",
         "-DNETSURF_BUILTIN_LOG_FILTER=\"level:WARNING\"",
         "-DNETSURF_BUILTIN_VERBOSE_FILTER=\"level:DEBUG\"",
         # Duktape reads its own configuration header, which NetSurf
         # supplies next to the engine.
         "-DDUK_OPT_HAVE_CUSTOM_H"]

INC = ["-I" + HOME + "/libc/include",
       "-I" + HOME + "/third_party/netsurf/include",
       "-I" + HOME + "/third_party/shim/include",
       "-I" + HOME + "/third_party/nsxyuos/nocurl",
       "-I" + NS, "-I" + NS + "/include",
       "-I" + NS + "/content/handlers",
       "-I" + NS + "/frontends",
       "-I" + NS + "/content/handlers/javascript/duktape",
       "-I" + DUK,                       # binding.h and generics.js.inc
       "-I/home/roman/src/ns/jsinc",     # where dukky.c expects duktape/
       "-I/home/roman/src/ns"]           # testament.h

os.makedirs(OUT, exist_ok=True)

# The engine, the glue NetSurf wraps round it, and every generated binding.
srcs = [NS + "/content/handlers/javascript/duktape/duktape.c",
        NS + "/content/handlers/javascript/duktape/dukky.c",
        NS + "/content/handlers/javascript/content.c"]

gen = sorted(f for f in os.listdir(DUK) if f.endswith(".c"))
srcs += [DUK + "/" + f for f in gen]

print("building %d files (%d of them generated bindings)\n"
      % (len(srcs), len(gen)))

missing = collections.Counter()
errors = collections.Counter()
objs, bad = [], 0

for c in srcs:
    obj = OUT + "/" + os.path.basename(c)[:-2] + ".o"
    r = subprocess.run([CC] + FLAGS + INC + ["-c", c, "-o", obj],
                       capture_output=True, text=True)
    if r.returncode == 0:
        objs.append(obj)
        continue
    bad += 1
    m = re.search(r"fatal error: ([^:]+): No such file", r.stderr)
    if m:
        missing[m.group(1).strip()] += 1
        continue
    for line in r.stderr.splitlines():
        if " error: " in line:
            errors[line.split(" error: ", 1)[1].strip()[:90]] += 1
            break

print("%d built, %d failed" % (len(objs), bad))
if missing:
    print("\nheaders it wants that are not here:")
    for h, n in missing.most_common():
        print("   %-30s x%d" % (h, n))
if errors:
    print("\ncomplaints:")
    for e, n in errors.most_common(12):
        print("   %-80s x%d" % (e, n))

if bad:
    sys.exit(1)

lib = HOME + "/third_party/nsxyuos/libnsjs.a"
if os.path.exists(lib):
    os.remove(lib)
subprocess.run([AR, "rcs", lib] + objs, check=True)
print("\n%s  %.1f MB" % (lib, os.path.getsize(lib) / 1e6))
