#!/usr/bin/env python3
"""Make exit() run what atexit() registered, and put the two new files in the
libc's source list."""
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


edit("libc/src/stdlib.c", "exit runs the handlers",
     """void exit(int code) { _exit(code); }""",
     """/* Defined in posixbits.c, next to atexit itself. */
void __libc_run_atexit(void);

void exit(int code) { __libc_run_atexit(); _exit(code); }""",
     "__libc_run_atexit")

edit("Makefile", "the new libc files",
     "libc/src/inet.c libc/src/regex.c",
     "libc/src/inet.c libc/src/regex.c \\\n             libc/src/timecal.c libc/src/posixbits.c",
     "libc/src/timecal.c")

print("--- done")
