#!/usr/bin/env python3
"""Tell NetSurf which of its fallbacks this libc already provides.

utils/utils.c carries its own strndup, scandir, uname, inet_aton and
inet_pton for systems without them, each behind a HAVE_ guard. This libc has
all five now, and defining nothing means two copies of each and a link that
fails on the first. The three it does NOT have -- strcasestr, strchrnul,
realpath -- are left undefined on purpose, so NetSurf's versions are used.
"""
R = "/home/roman/xyuos-neo/"
p = R + "tools/nscore.py"
s = open(p).read()

a = '''         "-DNETSURF_BUILTIN_VERBOSE_FILTER=\\"level:DEBUG\\""]'''
b = '''         "-DNETSURF_BUILTIN_VERBOSE_FILTER=\\"level:DEBUG\\"",
         # What this libc supplies, so NetSurf does not supply it twice.
         # strcasestr, strchrnul and realpath are deliberately absent from
         # this list: it does not have those, and NetSurf's own are wanted.
         "-DHAVE_STRNDUP", "-DHAVE_SCANDIR", "-DHAVE_UTSNAME",
         "-DHAVE_INETATON", "-DHAVE_INETPTON"]'''

if "HAVE_STRNDUP" in s:
    print("already")
else:
    assert a in s, "the flag list is not the shape expected"
    open(p, "w").write(s.replace(a, b, 1))
    print("ok: the HAVE_ flags")
