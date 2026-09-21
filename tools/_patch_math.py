#!/usr/bin/env python3
"""Declare cbrt, and put difftime back where it can be compiled.

difftime was taken out of time.h with a note saying a version returning a
long would be a different function wearing the name. That still holds -- what
changed is that it now lives in math.c, which is the one file in this libc
built with floating-point registers, so it can be the real one.
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
        raise SystemExit("MISSING in %s [%s]:\n%r" % (path, tag, a[:200]))
    open(R + path, "w").write(s.replace(a, b, 1))
    print("ok: %s (%s)" % (path, tag))


edit("libc/include/math.h", "cbrt",
     "double sqrt(double x);",
     """double sqrt(double x);
double cbrt(double x);""",
     "cbrt")

edit("libc/include/time.h", "difftime",
     """/* difftime is not here on purpose: it returns a double, and this libc is
 * built without floating-point registers. A version returning a long would
 * be a different function wearing the name. */""",
     """/* difftime returns a double, so it is defined in math.c -- the one file in
 * this libc compiled with floating-point registers. Declared here because
 * this is where callers look for it. */
double     difftime(time_t a, time_t b);""",
     "double     difftime")

# math.c needs time_t in scope for difftime's signature.
s = open(R + "libc/src/math.c").read()
if "#include <time.h>" in s:
    print("already: math.c sees time.h")
else:
    assert "#include <math.h>" in s
    open(R + "libc/src/math.c", "w").write(
        s.replace("#include <math.h>", "#include <math.h>\n#include <time.h>", 1))
    print("ok: math.c sees time.h")
