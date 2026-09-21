#!/usr/bin/env python3
"""Link NetSurf for xyuOS, and say plainly what is still missing.

This is the measurement that matters. Everything up to here proved that
files compile; a link proves the pieces refer to things that exist. Where
they do not, the undefined symbols are the remaining work, named.
"""
import os, re, subprocess, sys, collections

HOME = "/home/roman/xyuos-neo"
NS = "/home/roman/src/ns/netsurf-3.9"
BIN = HOME + "/toolchain/cross/bin/x86_64-elf-"
CORE_OBJ = "/home/roman/src/ns/core-obj"
OUT = "/home/roman/src/ns/link"

os.makedirs(OUT, exist_ok=True)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


# --- the core, as one archive ---------------------------------------------
objs = sorted(os.path.join(CORE_OBJ, f)
              for f in os.listdir(CORE_OBJ) if f.endswith(".o"))
if not objs:
    sys.exit("no core objects; run tools/nscore.py first")

core = OUT + "/libnscore.a"
if os.path.exists(core):
    os.remove(core)
r = run([BIN + "ar", "rcs", core] + objs)
if r.returncode != 0:
    sys.exit("could not archive the core:\n" + r.stderr)
print("core:      %d objects archived" % len(objs))

# --- everything, in an order the linker can resolve ------------------------
# The libraries are listed more than once on purpose: libcss calls into
# libwapcaplet, libdom calls into both, and a single pass over a static
# archive only takes what was already asked for.
LIBS = [
    HOME + "/third_party/nsxyuos/libnsglue.a",
    core,
    # Duktape and the generated DOM bindings. After the core, because it is
    # the core that calls into them.
    HOME + "/third_party/nsxyuos/libnsjs.a",
    HOME + "/third_party/netsurf/libdom.a",
    HOME + "/third_party/netsurf/libcss.a",
    HOME + "/third_party/netsurf/libhubbub.a",
    HOME + "/third_party/netsurf/libparserutils.a",
    HOME + "/third_party/netsurf/libwapcaplet.a",
    HOME + "/third_party/netsurf/libnsutils.a",
    HOME + "/third_party/netsurf/libnsbmp.a",
    HOME + "/third_party/netsurf/libutf8proc.a",
    HOME + "/third_party/shim/libnsshim.a",
    HOME + "/build/libc.a",
]
for p in LIBS:
    if not os.path.exists(p):
        sys.exit("missing: " + p)

elf = OUT + "/netsurf.elf"
cmd = ([BIN + "ld", "-nostdlib", "-static", "-o", elf,
        "-T", HOME + "/userland/link.ld", HOME + "/build/crt0.o"]
       + LIBS + LIBS + [HOME + "/build/libc.a"])

r = run(cmd)

if r.returncode == 0:
    size = os.path.getsize(elf)
    print("\nlinked:    %s, %d bytes" % (elf, size))
    sys.exit(0)

# --- what is missing -------------------------------------------------------
undef = collections.Counter()
other = []
for line in r.stderr.splitlines():
    m = re.search(r"undefined reference to [`']([^'\"]+)'", line)
    if m:
        undef[m.group(1)] += 1
    elif "error" in line.lower() or "cannot" in line.lower():
        other.append(line.strip())

print("\nthe link did not complete.\n")
if undef:
    print("%d name(s) nothing provides:\n" % len(undef))
    for name, n in undef.most_common():
        print("   %-40s wanted %d time(s)" % (name, n))
if other:
    print("\nand:")
    for line in other[:15]:
        print("   " + line)
sys.exit(1)
