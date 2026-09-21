#!/usr/bin/env python3
"""Give NetSurf a name in the start menu.

The menu lists everything in /bin whether or not it is named here, so this is
not what makes it appear -- it is what stops it appearing as "netsurf" with no
explanation, next to "Browser" which is a different program.
"""
R = "/home/roman/xyuos-neo/"
p = R + "kernel/wm/wm.c"
s = open(p).read()

a = '''    { "web",     "Browser",      "Open a page on the web" },'''
b = '''    { "web",     "Browser",      "Open a page on the web" },
    { "netsurf", "NetSurf",      "The NetSurf browser, ported here" },'''

if '"netsurf"' in s:
    print("already: netsurf is in the menu")
else:
    assert a in s, "the start menu table is not the shape expected"
    open(p, "w").write(s.replace(a, b, 1))
    print("ok: netsurf named in the start menu")
