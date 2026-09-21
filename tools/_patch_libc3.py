#!/usr/bin/env python3
"""Declare scandir, add mman.c to the libc, and compile NetSurf's javascript:
fetcher (the no-op one that goes with its no-op bindings)."""
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
        raise SystemExit("MISSING in %s [%s]:\n%r" % (path, tag, a[:200]))
    open(R + path, "w").write(s.replace(a, b, 1))
    print("ok: %s (%s)" % (path, tag))


edit("libc/include/dirent.h", "scandir",
     "void           rewinddir(DIR *d);",
     """void           rewinddir(DIR *d);

/* The whole listing at once, sorted. The caller frees each entry and then the
 * array. `filter` may be NULL to take everything; `compar` may be NULL to
 * leave the order alone, and alphasort is the usual one to pass. */
int  scandir(const char *path, struct dirent ***namelist,
             int (*filter)(const struct dirent *),
             int (*compar)(const struct dirent **, const struct dirent **));
int  alphasort(const struct dirent **a, const struct dirent **b);""",
     "scandir")

edit("Makefile", "mman.c",
     "libc/src/timecal.c libc/src/posixbits.c",
     "libc/src/timecal.c libc/src/posixbits.c libc/src/mman.c",
     "libc/src/mman.c")

edit("tools/nscore.py", "the javascript fetcher",
     'ALSO = ["content/handlers/javascript/none/none.c"]',
     '''ALSO = ["content/handlers/javascript/none/none.c",
        # And the javascript: URL fetcher, which the core registers whether
        # or not there is an engine behind it. Without a engine it answers
        # every such URL with nothing, which is the correct answer here.
        "content/handlers/javascript/fetcher.c"]''',
     "javascript/fetcher.c")

print("--- done")
