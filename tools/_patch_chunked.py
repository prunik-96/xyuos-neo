#!/usr/bin/env python3
"""Both transports now say the same thing about a rejoined body.

tls.c already undid the chunk framing; it just never said so, and net.c never
undid it at all. After this, both undo it and both rename the header, so the
absence of Transfer-Encoding means what a reader expects.
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

HOME = XYUOS


def patch(path, old, new, count=1):
    with open(path) as f:
        s = f.read()
    if new in s:
        print("   already there: " + path)
        return
    if s.count(old) != count:
        sys.exit("in %s: expected %d of\n%s\nfound %d"
                 % (path, count, old, s.count(old)))
    with open(path, "w") as f:
        f.write(s.replace(old, new))
    print("   patched " + path)


# --- tls.c: say so ---------------------------------------------------------
patch(HOME + "/kernel/net/tls.c",
      """        if (chunked) blen = http_dechunk(resp + hdr, blen);
""",
      """        if (chunked) {
            blen = http_dechunk(resp + hdr, blen);
            http_mark_dechunked(resp, hdr);
        }
""")

# --- net.c: do it at all ---------------------------------------------------
patch(HOME + "/kernel/net/net.c",
      """    int hdr = body;
    if (keep_headers) body = 0;
    int blen = tcp.rx_len - body;
    if (blen < 0) blen = 0;
    if (blen > max) blen = max;
    nmemcpy(buf, rx + body, blen);
    net_log_add(host, path, tcp.rx_len - hdr, status, (uint32_t)started, 0);
    return blen;
""",
      """    int hdr = body;

    // The body may have arrived in pieces, each behind its own length in
    // hexadecimal. Undone in place -- the headers sit in front of it and are
    // not disturbed -- and then the header that said so is renamed, because a
    // caller reading it would otherwise be told to undo it a second time.
    // This is the same treatment the TLS path gives it, from the same code.
    int blen = tcp.rx_len - hdr;
    if (blen < 0) blen = 0;
    if (http_is_chunked(rx, hdr)) {
        blen = http_dechunk((uint8_t *)rx + hdr, blen);
        http_mark_dechunked((uint8_t *)rx, hdr);
    }

    int n2 = keep_headers ? hdr + blen : blen;
    const uint8_t *src = keep_headers ? rx : rx + hdr;
    if (n2 > max) n2 = max;
    nmemcpy(buf, src, n2);
    net_log_add(host, path, blen, status, (uint32_t)started, 0);
    return n2;
""")

patch(HOME + "/kernel/net/net.c",
      """#include \"net.h\"
#include \"nic.h\"
""",
      """#include \"net.h\"
#include \"http.h\"
#include \"nic.h\"
""")

print("done")
