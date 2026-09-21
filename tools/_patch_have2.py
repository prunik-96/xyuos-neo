#!/usr/bin/env python3
"""Say which POSIX this libc claims, and let NetSurf work the rest out.

utils/config.h decides for itself which fallbacks to compile, and it undoes a
plain -DHAVE_STRNDUP on the next line. What it actually reads is
_POSIX_C_SOURCE: at 200809 it believes strndup is there and leaves its own
out, which is right -- this libc has it, and two definitions is a link error.

The same header already assumes scandir, uname, inet_aton and inet_pton on
anything that is not Windows, so those need nothing said; they are why those
four names came up as undefined earlier, and they are provided now.

It leaves strcasestr and strchrnul to _GNU_SOURCE, which is not claimed,
because this libc genuinely does not have them and NetSurf's own are wanted.
That is the whole point of not simply defining every HAVE_ in sight.
"""
R = "/home/roman/xyuos-neo/"
p = R + "tools/nscore.py"
s = open(p).read()

a = '''         # What this libc supplies, so NetSurf does not supply it twice.
         # strcasestr, strchrnul and realpath are deliberately absent from
         # this list: it does not have those, and NetSurf's own are wanted.
         "-DHAVE_STRNDUP", "-DHAVE_SCANDIR", "-DHAVE_UTSNAME",
         "-DHAVE_INETATON", "-DHAVE_INETPTON"]'''
b = '''         # Which POSIX this libc answers to. utils/config.h reads this to
         # decide whether to compile its own strndup; at 200809 it does not,
         # and the one in libc/src/string_extra.c is used instead. Claiming
         # _GNU_SOURCE as well would also switch off its strcasestr and
         # strchrnul, which this libc does NOT have -- so it is not claimed.
         "-D_POSIX_C_SOURCE=200809L"]'''

if "_POSIX_C_SOURCE" in s:
    print("already")
else:
    assert a in s, "the flag list is not the shape expected"
    open(p, "w").write(s.replace(a, b, 1))
    print("ok: _POSIX_C_SOURCE instead of the HAVE_ guesses")
