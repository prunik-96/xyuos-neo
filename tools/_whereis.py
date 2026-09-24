#!/usr/bin/env python3
"""Which function is this address in? For reading a panic."""
import os, subprocess, sys

XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NM = XYUOS + "/toolchain/cross/bin/x86_64-elf-nm"
ELF = XYUOS + "/build/xyuos_neo.elf"

want = int(sys.argv[1], 16)
out = subprocess.run([NM, "-n", ELF], capture_output=True, text=True).stdout

best = None
for line in out.splitlines():
    parts = line.split()
    if len(parts) != 3:
        continue
    addr, kind, name = parts
    if kind.lower() not in "tw":
        continue
    a = int(addr, 16)
    if a <= want:
        best = (a, name)
    else:
        if best:
            print("0x%x is %s + 0x%x   (next symbol %s at 0x%x)"
                  % (want, best[1], want - best[0], name, a))
            sys.exit(0)
        break
print("0x%x: %s" % (want, best[1] if best else "no symbol below it"))
