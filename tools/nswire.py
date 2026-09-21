#!/usr/bin/env python3
"""Put the half second on the wire and look at it.

Everything so far says the delay is inside tcp_connect and that the guest is
polling hard throughout, which leaves two possibilities: our SYN leaves late,
or the answer comes back late. Those are indistinguishable from inside the
guest and obvious from outside it, so this captures the packets QEMU carries
and times the handshakes.
"""
import http.server, os, socket, socketserver, struct, subprocess, sys, threading, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/nswire"
PCAP = OUT + "/net.pcap"
PORT = 8718
os.makedirs(OUT, exist_ok=True)

NSHEET = 8
PAGE = "<!doctype html><title>p</title>"
for i in range(NSHEET):
    PAGE += "<link rel='stylesheet' href='/s%d.css'>" % i
PAGE += "<h1>p</h1>"
SHEET = b".x { color: #123456; }"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        b = SHEET if self.path.endswith(".css") else PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type",
                         "text/css" if self.path.endswith(".css")
                         else "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = Server(("0.0.0.0", PORT), Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()

SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"
for f in (SER, MON, PCAP):
    try:
        os.unlink(f)
    except OSError:
        pass
subprocess.run(["cp", HOME + "/disk.img", OUT + "/disk.img"], check=True)

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host",
     "-cdrom", HOME + "/build/xyuos_neo.iso",
     "-serial", "file:" + SER, "-m", "512M", "-display", "none",
     "-drive", "file=%s/disk.img,if=none,id=d0,format=raw" % OUT,
     "-device", "virtio-blk-pci,drive=d0",
     "-device", "qemu-xhci,id=xhci",
     "-device", "usb-kbd,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     # Every frame in and out, with a timestamp.
     "-object", "filter-dump,id=f0,netdev=n0,file=" + PCAP,
     "-monitor", "unix:%s,server,nowait" % MON],
    stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def serial():
    try:
        with open(SER, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


t0 = time.time()
while "wm: double-buffer" not in serial():
    if time.time() - t0 > 120 or proc.poll() is not None:
        proc.kill()
        sys.exit("the window manager never came up")
    time.sleep(0.25)
time.sleep(4)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(60):
    try:
        s.connect(MON)
        break
    except OSError:
        time.sleep(0.25)
s.settimeout(0.3)


def cmd(c, settle=0.08):
    s.sendall((c + "\n").encode())
    time.sleep(settle)
    try:
        while True:
            if not s.recv(65536):
                break
    except socket.timeout:
        pass


KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '\n': 'ret',
        ':': 'shift-semicolon'}
for ch in "netsurf http://10.0.2.2:%d/p\n" % PORT:
    cmd("sendkey " + KEYS.get(ch, ch), 0.05)

time.sleep(20)
cmd("quit", 0.3)
time.sleep(1)
proc.kill()
srv.shutdown()

# --- read the capture ------------------------------------------------------
data = open(PCAP, "rb").read()
magic = struct.unpack("<I", data[:4])[0]
end = "<" if magic in (0xa1b2c3d4, 0xa1b23c4d) else ">"
nano = magic in (0xa1b23c4d, 0x4d3cb2a1)
off = 24

events = []          # (time, kind, sport, dport, flags)
while off + 16 <= len(data):
    ts, sub, caplen, origlen = struct.unpack(end + "IIII", data[off:off + 16])
    off += 16
    pkt = data[off:off + caplen]
    off += caplen
    t = ts + (sub / 1e9 if nano else sub / 1e6)

    if len(pkt) < 34 or pkt[12:14] != b"\x08\x00":
        continue
    ihl = (pkt[14] & 0x0F) * 4
    if pkt[14 + 9] != 6:                       # not TCP
        continue
    tcp = pkt[14 + ihl:]
    if len(tcp) < 14:
        continue
    sport, dport = struct.unpack(">HH", tcp[0:4])
    flags = tcp[13]
    events.append((t, sport, dport, flags))

if not events:
    sys.exit("nothing was captured")

base = events[0][0]
SYN, ACK, FIN, RST = 0x02, 0x10, 0x01, 0x04

# Pair each SYN from the guest with the SYN-ACK that answers it.
syns = {}
print("handshakes, timed on the wire:\n")
print("  %-8s %-8s %10s   %s" % ("port", "SYN at", "answer", "gap"))
gaps = []
for t, sp, dp, fl in events:
    if (fl & SYN) and not (fl & ACK) and dp == PORT:
        syns[sp] = t
    elif (fl & SYN) and (fl & ACK) and sp == PORT:
        if dp in syns:
            gap = (t - syns[dp]) * 1000
            gaps.append(gap)
            print("  %-8d %7.3fs %9.3fs   %6.0f ms"
                  % (dp, syns[dp] - base, t - base, gap))
            del syns[dp]

if gaps:
    print("\n%d handshake(s): quickest %.0f ms, slowest %.0f ms"
          % (len(gaps), min(gaps), max(gaps)))

# And whether anything is being retransmitted or refused.
rst = sum(1 for e in events if e[3] & RST)
print("resets seen: %d" % rst)

# Time from our FIN to the next SYN, which is what "the previous connection
# is still in the way" would look like.
print("\nall packets, first 40:")
for t, sp, dp, fl in events[:40]:
    names = "".join(n for b, n in ((0x02, "S"), (0x10, "A"), (0x01, "F"),
                                   (0x04, "R"), (0x08, "P")) if fl & b)
    who = "guest->host" if dp == PORT else "host->guest"
    print("  %7.3fs  %-11s %-5s port %d" % (t - base, who, names,
                                            sp if dp == PORT else dp))
