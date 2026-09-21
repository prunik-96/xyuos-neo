#!/usr/bin/env python3
"""Temporarily time the three parts of an HTTP request inside the kernel.

Connect, send, drain. The log already says a request took five hundred
milliseconds; this says which of the three spent them. Removed again by
running this with `off`.
"""
import sys
import os

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

R = XYUOS + "/"
p = R + "kernel/net/net.c"
s = open(p).read()

MARK = "// --- temporary: where a request's time goes ---"

ON = [
 ("""    uint64_t started = now_ms();
    if (!tcp_connect(ip, port)) {""",
  """    uint64_t started = now_ms();
""" + MARK + """
    uint64_t t_c0 = now_ms();
    if (!tcp_connect(ip, port)) {"""),

 ("""    tcp_send_data(req, n);
    if (postlen > 0) tcp_send_data(post, postlen);""",
  """    uint64_t t_c1 = now_ms();
    tcp_send_data(req, n);
    if (postlen > 0) tcp_send_data(post, postlen);
    uint64_t t_s1 = now_ms();"""),

 ("""    tcp_close();

    // Strip headers: find the blank line.""",
  """    kprintf("net: %s connect=%ums send=%ums drain=%ums\\n",
            path, (unsigned)(t_c1 - t_c0), (unsigned)(t_s1 - t_c1),
            (unsigned)(now_ms() - t_s1));
    tcp_close();

    // Strip headers: find the blank line."""),
]

if len(sys.argv) > 1 and sys.argv[1] == "off":
    if MARK not in s:
        print("already off")
        sys.exit(0)
    for a, b in ON:
        assert b in s, "cannot find the timing to remove:\n%r" % b[:120]
        s = s.replace(b, a, 1)
    open(p, "w").write(s)
    print("timing removed")
else:
    if MARK in s:
        print("already on")
        sys.exit(0)
    for a, b in ON:
        assert a in s, "cannot find where to add the timing:\n%r" % a[:120]
        s = s.replace(a, b, 1)
    open(p, "w").write(s)
    print("timing added")
