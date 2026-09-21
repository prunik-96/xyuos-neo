#!/usr/bin/env python3
"""Compile NetSurf's own core with the xyuOS toolchain and report the gap.

The libraries went through almost untouched. The core is where a browser
stops being a parser and starts being a program: it reads files, keeps a
cache, asks the clock what time it is, and expects a platform underneath it.
This finds out how much of that platform xyuOS already is.

The frontends are left out on purpose -- one of those has to be written for
xyuOS regardless, and counting GTK's failures would say nothing.
"""
import os, re, subprocess, sys, collections

HOME = "/home/roman/xyuos-neo"
NS = "/home/roman/src/ns/netsurf-3.9"
CC = HOME + "/toolchain/cross/bin/x86_64-elf-gcc"
STAGE = HOME + "/third_party/netsurf/include"
OUT = "/home/roman/src/ns/core-obj"

# The same flags its own libraries were built with, plus what NetSurf's build
# would have defined for a build with no optional extras.
FLAGS = ["-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
         "-mno-red-zone", "-msse", "-msse2", "-std=gnu11", "-O2", "-w",
         "-fcommon", "-D_ALIGNED=",
         "-DWITH_CSS", "-DWITH_HTML", "-DWITH_TEXT",
         "-Dnsxyuos", "-DNETSURF_UA_FORMAT_STRING=\"%s\"",
         "-DNETSURF_HOMEPAGE=\"about:welcome\"",
         # Which log messages are compiled in at all. Its own build sets this
         # from a configuration option; nothing chooses it for us.
         "-DNETSURF_BUILTIN_LOG_FILTER=\"level:WARNING\"",
         "-DNETSURF_BUILTIN_VERBOSE_FILTER=\"level:DEBUG\"",
         # Which POSIX this libc answers to. utils/config.h reads this to
         # decide whether to compile its own strndup; at 200809 it does not,
         # and the one in libc/src/string_extra.c is used instead. Claiming
         # _GNU_SOURCE as well would also switch off its strcasestr and
         # strchrnul, which this libc does NOT have -- so it is not claimed.
         "-D_POSIX_C_SOURCE=200809L"]

DIRS = ["content", "desktop", "utils"]

# Decoders and bindings NetSurf is built WITHOUT unless asked: each wants a
# library of its own, and a build that does not enable them never compiles
# these files. Counting them as failures would be counting a choice.
OPTIONAL = ("image/png.c", "image/gif.c", "image/bmp.c", "image/ico.c",
            "image/jpeg.c", "image/webp.c", "image/rsvg.c", "image/svg.c",
            "image/nssprite.c", "image/video.c",
            "javascript/", "fetchers/curl.c")

# Except for this one. NetSurf ships the no-op form of its script bindings for
# builds without an engine, and the core calls them unconditionally -- so it
# is not optional at all, it is the other half of leaving JavaScript out.
# The javascript: URL fetcher, which the core registers whether or not there
# is an engine behind it.
#
# The no-op bindings that used to be here -- javascript/none/none.c -- are
# gone: Duktape and its generated bindings are built by tools/nsjs.py and
# define the same names for real. Keeping the stubs would be two definitions
# of js_initialise, js_exec and everything else.
ALSO = ["content/handlers/javascript/fetcher.c"]

os.makedirs(OUT, exist_ok=True)

# Half of what looked missing was never missing: the core writes
# `#include "css/utils.h"` for a header that lives in content/handlers/css,
# because that directory is on its own build's path. A survey that leaves it
# off measures the survey, not the port.
inc = ["-I" + HOME + "/libc/include", "-I" + STAGE, "-I" + HOME + "/third_party/shim/include",
       "-I" + NS, "-I" + NS + "/include",
       "-I" + NS + "/content/handlers",
       "-I" + NS + "/frontends",
       "-I/home/roman/src/ns",                      # where testament.h was generated
       # Two directories that exist to let files be read, not to add
       # behaviour: nocurl names one type so that content/fetch.c parses (see
       # the header), and the javascript/none directory is NetSurf own set of
       # no-op bindings for a build without a script engine.
       "-I" + HOME + "/third_party/nsxyuos/nocurl"]

missing = collections.Counter()
errors = collections.Counter()
per_dir = {}
symbols = collections.Counter()
skipped = []

for d in DIRS:
    srcs = []
    for base, _, files in os.walk(NS + "/" + d):
        for f in sorted(files):
            if not f.endswith(".c"):
                continue
            rel = os.path.relpath(os.path.join(base, f), NS + "/" + d)
            if any(o in rel.replace("\\", "/") for o in OPTIONAL):
                skipped.append(rel)
                continue
            srcs.append(os.path.join(base, f))
    for extra in ALSO:
        if extra.startswith(d + "/"):
            srcs.append(NS + "/" + extra)
    ok = bad = 0
    for c in srcs:
        o = OUT + "/" + os.path.relpath(c, NS).replace("/", "_")[:-2] + ".o"
        r = subprocess.run([CC] + FLAGS + inc + ["-c", c, "-o", o],
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
        for line in err.splitlines():
            if " error: " in line:
                msg = line.split(" error: ", 1)[1].strip()
                # Group the "undeclared" ones by the name, since that is the
                # thing that has to be provided.
                u = re.match(r"'([A-Za-z_][A-Za-z0-9_]*)' undeclared", msg)
                if u:
                    symbols[u.group(1)] += 1
                else:
                    errors[msg[:88]] += 1
                break
    per_dir[d] = (ok, bad, len(srcs))

print("NetSurf's core, built against xyuOS's libc:\n")
tot_ok = tot_bad = 0
for d in DIRS:
    ok, bad, n = per_dir[d]
    tot_ok += ok
    tot_bad += bad
    print("   %-12s %3d of %3d built, %3d failed" % (d, ok, n, bad))
print("\n   %-12s %3d built, %3d failed" % ("in total", tot_ok, tot_bad))
print("   %-12s %3d left out (a decoder or a binding this build does not want)"
      % ("", len(skipped)))

if missing:
    print("\nHeaders it wants that are not here:")
    for h, n in missing.most_common(25):
        print("   %-30s wanted by %2d file(s)" % (h, n))

if symbols:
    print("\nNames it uses that nothing declares:")
    for sname, n in symbols.most_common(25):
        print("   %-30s in %2d file(s)" % (sname, n))

if errors:
    print("\nEverything else:")
    for e, n in errors.most_common(20):
        print("   %2d x  %s" % (n, e))
