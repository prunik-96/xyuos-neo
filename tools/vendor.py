#!/usr/bin/env python3
"""Put back the two trees that belong to other people.

DOOM and MicroPython are clones, not copies: they carry their own history and
they are large, so the repository pins them by commit here instead of holding
them. This restores exactly the commits the port was built and tested against.
Nobody's `main` moving under us is not an upgrade, it is a different porting
job, so the commits are exact and not a branch name.

Nothing of ours lives inside either of them, and that is deliberate: git
records a nested repository as a link and refuses to look inside, so anything
we kept in there would silently never be committed. Our MicroPython port is
third_party/mpy-xyuos and reaches into their tree through MPY_TOP; our DOOM
platform layer is userland/doomgeneric_xyuos.c.

    python3 tools/vendor.py          restore whatever is missing
    python3 tools/vendor.py --check  say what is missing, change nothing
"""
import os, shutil, subprocess, sys

HOME = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VENDOR = [
    {
        "path": "third_party/doomgeneric",
        "url": "https://github.com/ozkl/doomgeneric.git",
        "commit": "dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284",
        "why": "DOOM. Our platform layer is userland/doomgeneric_xyuos.c.",
    },
    {
        "path": "third_party/micropython",
        "url": "https://github.com/micropython/micropython.git",
        "commit": "1c3c201149f37fe8d81246191b3127bb198d6306",
        "why": "MicroPython. Our port is third_party/mpy-xyuos and finds "
               "this tree through MPY_TOP.",
    },
]

CHECK = "--check" in sys.argv


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    return r.stdout.strip()


def head_of(path):
    r = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


missing = 0
for v in VENDOR:
    full = os.path.join(HOME, v["path"])
    at = head_of(full) if os.path.isdir(os.path.join(full, ".git")) else None

    if at == v["commit"]:
        print("%-30s already at %s" % (v["path"], v["commit"][:12]))
        continue

    if at:
        print("%-30s is at %s, wanted %s" % (v["path"], at[:12],
                                             v["commit"][:12]))
        if CHECK:
            missing += 1
            continue
        run(["git", "-C", full, "fetch", "--depth", "50", "origin",
             v["commit"]])
        run(["git", "-C", full, "checkout", "--detach", v["commit"]])
        print("%-30s moved to %s" % (v["path"], v["commit"][:12]))
        continue

    print("%-30s MISSING -- %s" % (v["path"], v["why"]))
    missing += 1
    if CHECK:
        continue

    if os.path.isdir(full) and not os.listdir(full):
        os.rmdir(full)
    if os.path.isdir(full):
        sys.exit("%s exists and is not a clone; move it aside first" % full)

    print("   cloning %s" % v["url"])
    run(["git", "clone", "--filter=blob:none", v["url"], full])
    run(["git", "-C", full, "checkout", "--detach", v["commit"]])
    print("%-30s restored at %s" % (v["path"], v["commit"][:12]))

if CHECK and missing:
    print("\n%d tree(s) missing; run without --check to restore them." % missing)
    sys.exit(1)
print("\nvendored trees are in place")
