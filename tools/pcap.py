#!/usr/bin/env python3
"""Minimal pcap reader: print the TCP conversation on a port, with a one-line
summary of each TLS record so a failed handshake can be read at a glance."""
import struct, sys

path = sys.argv[1]
want_port = int(sys.argv[2]) if len(sys.argv) > 2 else 443

data = open(path, "rb").read()
magic = struct.unpack("<I", data[:4])[0]
endian = "<" if magic in (0xa1b2c3d4, 0xa1b23c4d) else ">"
off = 24

REC = {20: "ChangeCipherSpec", 21: "Alert", 22: "Handshake", 23: "AppData"}
HS  = {1: "ClientHello", 2: "ServerHello", 4: "NewSessionTicket", 8: "EncryptedExtensions",
       11: "Certificate", 13: "CertificateRequest", 15: "CertificateVerify", 20: "Finished"}

streams = {}
while off + 16 <= len(data):
    ts, tus, caplen, origlen = struct.unpack(endian + "IIII", data[off:off+16])
    off += 16
    pkt = data[off:off+caplen]
    off += caplen
    if len(pkt) < 34 or pkt[12:14] != b"\x08\x00":
        continue
    ip = pkt[14:]
    ihl = (ip[0] & 0xF) * 4
    if ip[9] != 6:
        continue
    src = ".".join(str(b) for b in ip[12:16])
    dst = ".".join(str(b) for b in ip[16:20])
    tcp = ip[ihl:]
    sport, dport = struct.unpack(">HH", tcp[0:4])
    doff = (tcp[12] >> 4) * 4
    flags = tcp[13]
    payload = tcp[doff:]
    if want_port not in (sport, dport):
        continue
    fl = "".join(n for b, n in ((0x02, "S"), (0x10, "A"), (0x08, "P"), (0x01, "F"), (0x04, "R")) if flags & b)
    print("%s:%d -> %s:%d  %-4s len=%d" % (src, sport, dst, dport, fl, len(payload)))
    key = (src, sport, dst, dport)
    streams[key] = streams.get(key, b"") + payload

for key, buf in streams.items():
    if not buf:
        continue
    print("\n=== %s:%d -> %s:%d, %d bytes ===" % (key + (len(buf),)))
    i = 0
    while i + 5 <= len(buf):
        t, ver, ln = buf[i], struct.unpack(">H", buf[i+1:i+3])[0], struct.unpack(">H", buf[i+3:i+5])[0]
        body = buf[i+5:i+5+ln]
        note = ""
        if t == 22 and body:
            note = " " + HS.get(body[0], "hs type %d" % body[0])
        if t == 21 and len(body) >= 2:
            note = " level=%d desc=%d" % (body[0], body[1])
        print("  record type=%s ver=0x%04x len=%d%s" % (REC.get(t, t), ver, ln, note))
        i += 5 + ln
    if i < len(buf):
        print("  (%d trailing bytes, not a complete record)" % (len(buf) - i))
