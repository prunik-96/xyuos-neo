#!/usr/bin/env python3
"""Fetch and unpack everything the NetSurf port is built from.

This exists because the sources were in /tmp, and /tmp does not survive here:
this WSL clears it between sessions, so the port was one reboot away from
being unbuildable. Everything now lives under ~/src, and this script puts it
back from nothing.
"""
import os, subprocess, sys, tarfile

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

SRC = XYUOS_SRC + "/ns"
STAGE = XYUOS + "/third_party/netsurf"

BASE = "https://download.netsurf-browser.org"

# The core, and the eight libraries it is built on. Versions pinned: this is
# what the port was measured against, and a newer libcss is a different
# porting job, not a free upgrade.
WANT = [
    ("netsurf-3.9-src.tar.gz",          BASE + "/netsurf/releases/source/"),
    ("libwapcaplet-0.4.3-src.tar.gz",   BASE + "/libs/releases/"),
    ("libparserutils-0.2.4-src.tar.gz", BASE + "/libs/releases/"),
    ("libhubbub-0.3.8-src.tar.gz",      BASE + "/libs/releases/"),
    ("libcss-0.9.1-src.tar.gz",         BASE + "/libs/releases/"),
    ("libdom-0.4.2-src.tar.gz",         BASE + "/libs/releases/"),
    ("libnsutils-0.1.0-src.tar.gz",     BASE + "/libs/releases/"),
    ("libnsbmp-0.1.6-src.tar.gz",       BASE + "/libs/releases/"),
    ("libutf8proc-2.4.0-1-src.tar.gz",  BASE + "/libs/releases/"),
]

os.makedirs(SRC, exist_ok=True)

got = missing = 0
for name, where in WANT:
    path = os.path.join(SRC, name)
    if not os.path.exists(path):
        r = subprocess.run(["curl", "-sfL", "-o", path, where + name],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(path):
            print("could not fetch %s" % name)
            missing += 1
            continue
    got += 1

    # Unpack once; leave it alone afterwards so local edits survive.
    with tarfile.open(path) as t:
        top = t.getnames()[0].split("/")[0]
        if not os.path.isdir(os.path.join(SRC, top)):
            t.extractall(SRC)
            print("unpacked %s" % top)
        else:
            print("already there: %s" % top)

print("\n%d archive(s) in %s%s"
      % (got, SRC, ", %d could not be fetched" % missing if missing else ""))

# testament.h is generated, not shipped, and two files in the core include it.
# NetSurf's own build makes it with this script; forgetting it costs two
# failures that look like missing system headers and are not.
core = os.path.join(SRC, "netsurf-3.9")
testament = os.path.join(SRC, "testament.h")
if os.path.isdir(core) and not os.path.exists(testament):
    r = subprocess.run(["perl", "utils/git-testament.pl", ".", testament],
                       cwd=core, capture_output=True, text=True)
    if os.path.exists(testament):
        print("generated testament.h")
    else:
        print("could not generate testament.h:\n" + r.stdout + r.stderr)

if missing == 0:
    print("\nnow: tools/nsbuild.py, nsshim.py, nscore.py, nsglue.py, "
          "nslink.py, nsres.py, nsinstall.py, then make.")
