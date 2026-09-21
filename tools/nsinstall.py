#!/usr/bin/env python3
"""Put NetSurf into the real disc image, so it is there like any other program.

Everything up to now built it into /tmp and copied it onto a throwaway disc
for one test run. This is the step that installs it: the binary in /bin, its
stylesheets and wording in /lib/netsurf. After this the start menu lists it,
because the menu reads /bin rather than a table of what ought to be there.

Run tools/nslink.py and tools/nsres.py first. `make` afterwards, so the ISO
picks up the changed disc image -- on real hardware the disc IS the ISO's
module, and an ISO built before this step does not have it.
"""
import os, subprocess, sys

HOME = "/home/roman/xyuos-neo"
ELF = "/home/roman/src/ns/link/netsurf.elf"
RES = HOME + "/third_party/nsxyuos/res"
IMG = HOME + "/disk.img"
STRIP = HOME + "/toolchain/cross/bin/x86_64-elf-strip"

if not os.path.exists(ELF):
    sys.exit("no binary at %s -- run tools/nslink.py first" % ELF)
if not os.path.isdir(RES):
    sys.exit("no resources at %s -- run tools/nsres.py first" % RES)
if not os.path.exists(IMG):
    sys.exit("no disc image at %s -- run make first" % IMG)


def debugfs(cmd, quiet=False):
    r = subprocess.run(["debugfs", "-w", "-R", cmd, IMG],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    if not quiet and "Allocated inode" not in out and "rm" not in cmd \
            and "mkdir" not in cmd:
        print("   (debugfs said: %s)" % out.strip().splitlines()[-1:])
    return out


# The debug information is four times the program; the disc is thirty-two
# megabytes. Same reasoning as python.elf in the Makefile.
stripped = HOME + "/third_party/nsxyuos/netsurf.elf"
subprocess.run([STRIP, "-o", stripped, ELF], check=True)

debugfs("rm /bin/netsurf", quiet=True)
debugfs("write %s /bin/netsurf" % stripped)
print("netsurf: %d bytes in /bin" % os.path.getsize(stripped))

debugfs("mkdir /lib/netsurf", quiet=True)
n = 0
for f in sorted(os.listdir(RES)):
    src = os.path.join(RES, f)
    if not os.path.isfile(src):
        continue
    debugfs("rm /lib/netsurf/" + f, quiet=True)
    out = debugfs("write %s /lib/netsurf/%s" % (src, f), quiet=True)
    if "Allocated inode" in out:
        n += 1
    else:
        print("   could not write %s: %s" % (f, out.strip()))
print("resources: %d files in /lib/netsurf" % n)

# And the icons, which live in a subdirectory because that is the name
# NetSurf asks for them under: resource:icons/search.png.
ICONS = os.path.join(RES, "icons")
if os.path.isdir(ICONS):
    debugfs("mkdir /lib/netsurf/icons", quiet=True)
    k = 0
    for f in sorted(os.listdir(ICONS)):
        src = os.path.join(ICONS, f)
        if not os.path.isfile(src):
            continue
        debugfs("rm /lib/netsurf/icons/" + f, quiet=True)
        if "Allocated inode" in debugfs(
                "write %s /lib/netsurf/icons/%s" % (src, f), quiet=True):
            k += 1
    print("icons: %d files in /lib/netsurf/icons" % k)

# Say plainly what is left to do, because forgetting it is the obvious trap.
print("\nnow run `make` so the ISO carries this disc image.")
