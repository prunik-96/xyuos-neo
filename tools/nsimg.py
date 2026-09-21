#!/usr/bin/env python3
"""Does it show pictures now.

Serves a page with a PNG, a JPEG and an SVG on it, and photographs what the
browser makes of them. The pictures are generated here rather than taken from
somewhere, so the right answer is known: three coloured shapes of known size.
"""
import http.server, os, socket, socketserver, struct, subprocess, sys, threading, time, zlib

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT = "/tmp/nsimg"
PORT = 8717
os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)


def make_png(w, h, fn):
    """A PNG, written out by hand: the format is a header, an IHDR, the rows
    zlib-compressed with a filter byte each, and an IEND."""
    raw = b""
    for y in range(h):
        raw += b"\x00"
        for x in range(w):
            raw += bytes(fn(x, y))

    def chunk(tag, body):
        c = tag + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I",
                                                              zlib.crc32(c))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


PNG = make_png(160, 120, lambda x, y: (x * 255 // 160, y * 255 // 120, 128))

SVG = ("<svg xmlns='http://www.w3.org/2000/svg' width='160' height='120'>"
       "<rect width='160' height='120' fill='#2a6'/>"
       "<circle cx='80' cy='60' r='45' fill='#fd3'/>"
       "<rect x='55' y='35' width='50' height='50' fill='#a33'/>"
       "</svg>").encode()

PAGE = ("<!doctype html><title>pictures</title>"
        "<style>body{font-family:sans-serif;margin:20px}"
        "img{border:2px solid #333;margin:8px}</style>"
        "<h1>three pictures</h1>"
        "<p>a PNG gradient, then the same drawn as SVG:</p>"
        "<p><img src='/a.png' width='160' height='120' alt='the png'>"
        "<img src='/b.svg' width='160' height='120' alt='the svg'></p>"
        "<p>and one scaled down by the page:</p>"
        "<p><img src='/a.png' width='80' height='60' alt='small'></p>"
        ).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.endswith(".png"):
            b, t = PNG, "image/png"
        elif self.path.endswith(".svg"):
            b, t = SVG, "image/svg+xml"
        else:
            b, t = PAGE, "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", t)
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
for f in (SER, MON):
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
print("the system is up")
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
cmd("screendump %s/01_pictures.ppm" % OUT, 1.2)

log = serial()
bad = [l for l in log.splitlines()
       if "PANIC" in l or "FAULT" in l or "#PF" in l or "#GP" in l]
print("panics/faults:", "\n   ".join(bad) if bad else "(none)")

cmd("quit", 0.3)
time.sleep(1)
proc.kill()
srv.shutdown()

for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("pictures in " + OUT)
