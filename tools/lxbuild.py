#!/usr/bin/env python3
"""Compile Lexbor with the xyuOS toolchain and say what it wants.

The same measurement the NetSurf port started with, and for the same reason:
a library's README says what it depends on, and the compiler says what it
actually depends on. Only one of those two is worth planning against.

Lexbor keeps its platform-specific code in source/lexbor/ports/, one small
directory per system. This builds against the posix one to find out how much
of posix it really touches -- what fails here is the list of things a third
port directory would have to answer for.
"""
import os, re, subprocess, sys, collections

HOME = "/home/roman/xyuos-neo"
LX = "/home/roman/src/lexbor-2.4.0"
CC = HOME + "/toolchain/cross/bin/x86_64-elf-gcc"
AR = HOME + "/toolchain/cross/bin/x86_64-elf-ar"
OUT = "/home/roman/src/lx-obj"

FLAGS = ["-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
         "-mno-red-zone", "-msse", "-msse2", "-std=gnu11", "-O2", "-w",
         "-fcommon",
         # Lexbor builds one static library by default; saying so switches
         # off the visibility attributes meant for a shared one.
         "-DLEXBOR_STATIC"]

INC = ["-I" + HOME + "/libc/include",
       "-I" + LX + "/source",
       "-I" + LX + "/source/lexbor/ports/posix"]

os.makedirs(OUT, exist_ok=True)

# Everything except the other systems' port directories: those are for
# Windows, and counting their failures would measure nothing.
srcs = []
for base, _, files in os.walk(LX + "/source"):
    rel = os.path.relpath(base, LX + "/source")
    if "ports/windows_nt" in rel.replace("\\", "/"):
        continue
    for f in sorted(files):
        if f.endswith(".c"):
            srcs.append(os.path.join(base, f))
srcs.sort()

missing = collections.Counter()
errors = collections.Counter()
per_module = collections.defaultdict(lambda: [0, 0])
objs = []

for c in srcs:
    rel = os.path.relpath(c, LX + "/source/lexbor")
    module = rel.split("/")[0] if "/" in rel else "(top)"
    obj = OUT + "/" + rel.replace("/", "_")[:-2] + ".o"

    r = subprocess.run([CC] + FLAGS + INC + ["-c", c, "-o", obj],
                       capture_output=True, text=True)
    if r.returncode == 0:
        per_module[module][0] += 1
        objs.append(obj)
        continue

    per_module[module][1] += 1
    m = re.search(r"fatal error: ([^:]+): No such file", r.stderr)
    if m:
        missing[m.group(1).strip()] += 1
        continue
    for line in r.stderr.splitlines():
        if " error: " in line:
            errors[line.split(" error: ", 1)[1].strip()[:80]] += 1
            break

print("Lexbor, built against xyuOS's libc:\n")
ok = bad = 0
for module in sorted(per_module):
    o, b = per_module[module]
    ok += o
    bad += b
    print("   %-12s %3d of %3d built%s"
          % (module, o, o + b, ", %d failed" % b if b else ""))
print("\n   %-12s %3d built, %3d failed" % ("in total", ok, bad))

if missing:
    print("\nHeaders it wants that are not here:")
    for h, n in missing.most_common():
        print("   %-30s wanted by %2d file(s)" % (h, n))
if errors:
    print("\nOther complaints:")
    for e, n in errors.most_common(12):
        print("   %-70s x%d" % (e, n))

if bad:
    sys.exit(1)

lib = "/home/roman/src/liblexbor.a"
if os.path.exists(lib):
    os.remove(lib)
subprocess.run([AR, "rcs", lib] + objs, check=True)
print("\n%s  %.1f MB" % (lib, os.path.getsize(lib) / 1e6))

NM = HOME + "/toolchain/cross/bin/x86_64-elf-nm"


def symbols(path, defined):
    """Names an archive defines, or names it leaves undefined."""
    args = [NM, "--defined-only", "-g", path] if defined else [NM, "-u", path]
    out = subprocess.run(args, capture_output=True, text=True).stdout
    found = set()
    for line in out.splitlines():
        line = line.strip()
        if not line or line.endswith(":"):        # the "file.o:" headings
            continue
        bits = line.split()
        if defined:
            # "address TYPE name"; only real definitions count.
            if len(bits) >= 3 and bits[-2] in ("T", "D", "B", "R", "W"):
                found.add(bits[-1])
        else:
            found.add(bits[-1])
    return found


# An archive's objects refer to each other, so "undefined" per object counts
# most of the library itself. What matters is what is undefined across the
# whole of it -- that is the list the system has to answer for.
wants = symbols(lib, False) - symbols(lib, True)
print("\nnames it needs from outside itself (%d):" % len(wants))
print("   " + " ".join(sorted(wants)))

provided = symbols(HOME + "/build/libc.a", True)
gaps = sorted(wants - provided)
print("\nof those, NOT in this libc (%d):" % len(gaps))
print("   " + (" ".join(gaps) if gaps else "(none -- it links as it stands)"))
