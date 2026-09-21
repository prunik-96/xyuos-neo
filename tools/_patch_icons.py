#!/usr/bin/env python3
"""Put the icon subdirectory on the disc as well.

Both the installer and the Makefile copy the resources as a flat list of
files, so a subdirectory was silently skipped -- which is why the icons were
collected and still did not appear.
"""
import os

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))
R = XYUOS + "/"


def edit(path, tag, a, b, marker):
    s = open(R + path).read()
    if marker in s:
        print("already: %s (%s)" % (path, tag))
        return
    if a not in s:
        raise SystemExit("MISSING in %s [%s]:\n%r" % (path, tag, a[:250]))
    open(R + path, "w").write(s.replace(a, b, 1))
    print("ok: %s (%s)" % (path, tag))


edit("tools/nsinstall.py", "the icons",
     """print("resources: %d files in /lib/netsurf" % n)""",
     """print("resources: %d files in /lib/netsurf" % n)

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
    print("icons: %d files in /lib/netsurf/icons" % k)""",
     "/lib/netsurf/icons")

edit("Makefile", "the icons",
     """	    for f in third_party/nsxyuos/res/*; do \\
	        debugfs -w -R "write $$f /lib/netsurf/`basename $$f`" disk.img >/dev/null 2>&1; \\
	    done; \\""",
     """	    for f in third_party/nsxyuos/res/*; do \\
	        test -f $$f && debugfs -w -R "write $$f /lib/netsurf/`basename $$f`" disk.img >/dev/null 2>&1; \\
	    done; \\
	    debugfs -w -R "mkdir /lib/netsurf/icons" disk.img >/dev/null 2>&1; \\
	    for f in third_party/nsxyuos/res/icons/*; do \\
	        test -f $$f && debugfs -w -R "write $$f /lib/netsurf/icons/`basename $$f`" disk.img >/dev/null 2>&1; \\
	    done; \\""",
     "/lib/netsurf/icons")

print("--- done")
