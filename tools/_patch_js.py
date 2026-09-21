#!/usr/bin/env python3
"""Switch the core from the no-op script bindings to the real engine.

content/handlers/javascript/none/none.c defines js_initialise, js_exec and
the rest as functions that do nothing -- the shape NetSurf takes when it is
built without an engine. dukky.c defines the same names for real. Both in one
link is two definitions of everything, so the stubs come out.

content.c is in the javascript library now rather than the core, for the same
reason: it is compiled there with the engine's headers on the path.
"""
R = "/home/roman/xyuos-neo/"
p = R + "tools/nscore.py"
s = open(p).read()

A = '''ALSO = ["content/handlers/javascript/none/none.c",
        # And the javascript: URL fetcher, which the core registers whether
        # or not there is an engine behind it. Without a engine it answers
        # every such URL with nothing, which is the correct answer here.
        "content/handlers/javascript/fetcher.c"]'''

B = '''# The javascript: URL fetcher, which the core registers whether or not there
# is an engine behind it.
#
# The no-op bindings that used to be here -- javascript/none/none.c -- are
# gone: Duktape and its generated bindings are built by tools/nsjs.py and
# define the same names for real. Keeping the stubs would be two definitions
# of js_initialise, js_exec and everything else.
ALSO = ["content/handlers/javascript/fetcher.c"]'''

if "no-op bindings that used to be here" in s:
    print("already: the stubs are out")
else:
    assert A in s, "nscore.py is not the shape expected"
    open(p, "w").write(s.replace(A, B, 1))
    print("ok: the no-op bindings are no longer built")

# And the link needs the new archive.
p = R + "tools/nslink.py"
s = open(p).read()

A2 = '''    HOME + "/third_party/nsxyuos/libnsglue.a",
    core,'''
B2 = '''    HOME + "/third_party/nsxyuos/libnsglue.a",
    core,
    # Duktape and the generated DOM bindings. After the core, because it is
    # the core that calls into them.
    HOME + "/third_party/nsxyuos/libnsjs.a",'''

if "libnsjs.a" in s:
    print("already: the engine is in the link")
else:
    assert A2 in s, "nslink.py is not the shape expected"
    open(p, "w").write(s.replace(A2, B2, 1))
    print("ok: the engine is in the link")
