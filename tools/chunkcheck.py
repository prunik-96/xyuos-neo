#!/usr/bin/env python3
"""Run the fetcher's dechunker over a real response and say where it stops.

The same walk as dechunk() in fetch_xyuos.c, written here so it can be tried
against bytes a real server sent without booting anything. A dechunker that
is wrong is invisible from inside the browser -- the page is simply blank --
so it is worth being able to point at the exact byte it gave up on.
"""
import subprocess, sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "github.com"

req = ("GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
       "Accept-Encoding: gzip, deflate, identity\r\n"
       "User-Agent: xyuos\r\n\r\n" % HOST)

r = subprocess.run(["openssl", "s_client", "-quiet", "-connect", HOST + ":443",
                    "-servername", HOST],
                   input=req.encode(), capture_output=True, timeout=40)
d = r.stdout
if not d:
    sys.exit("nothing came back from " + HOST)

cut = d.find(b"\r\n\r\n")
if cut < 0:
    sys.exit("no header block")
head, body = d[:cut], d[cut + 4:]

print("%s: %d bytes of headers, %d of body" % (HOST, len(head) + 4, len(body)))
for line in head.split(b"\r\n"):
    low = line.lower()
    if low.startswith((b"transfer-encoding:", b"content-encoding:",
                       b"content-length:", b"http/")):
        print("   " + line.decode("latin-1"))

print("\nthe body begins: %r" % body[:48])
print("the body ends:   %r" % body[-48:])

# --- the same walk dechunk() does -----------------------------------------
p, n = body, len(body)
i = out = 0
pieces = 0
joined = bytearray()

while True:
    start = i
    while i < n and p[i] != 0x0A:
        i += 1
    if i >= n:
        print("\nSTOPPED: ran out of bytes looking for the end of a size line"
              " at %d of %d" % (start, n))
        break

    size = 0
    digits = 0
    for k in range(start, i):
        ch = p[k]
        if 0x30 <= ch <= 0x39:
            v = ch - 0x30
        elif 0x61 <= ch <= 0x66:
            v = ch - 0x61 + 10
        elif 0x41 <= ch <= 0x46:
            v = ch - 0x41 + 10
        else:
            break
        size = size * 16 + v
        digits += 1

    if digits == 0:
        print("\nSTOPPED at byte %d: expected a size in hexadecimal, found %r"
              % (start, bytes(p[start:start + 24])))
        break
    i += 1

    if size == 0:
        print("\nreached the final piece after %d pieces" % pieces)
        break
    if i + size > n:
        print("\nSTOPPED: piece %d says %d bytes but only %d are left"
              % (pieces, size, n - i))
        break

    joined += p[i:i + size]
    i += size
    pieces += 1
    while i < n and p[i] != 0x0A:
        i += 1
    i += 1

print("joined: %d bytes from %d pieces" % (len(joined), pieces))
if joined[:2] == b"\x1f\x8b":
    import zlib
    try:
        html = zlib.decompress(bytes(joined), 16 + zlib.MAX_WBITS)
        print("and it inflates to %d bytes of HTML" % len(html))
        print("which begins: %r" % html[:60])
    except Exception as e:
        print("but it does not inflate: %s" % e)
else:
    print("it does not start with the gzip magic: %r" % bytes(joined[:8]))
