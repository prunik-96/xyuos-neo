#!/usr/bin/env python3
"""The same Russian sentence served in four encodings, declared four ways.

A page in the wrong encoding does not fail -- it renders, in nonsense. So the
only test worth running is to look at it, and the four pages carry the same
words so that three of them being right and one wrong is obvious.
"""
import http.server, os, socket, socketserver, subprocess, sys, threading, time

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT = "/tmp/enctest"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"

RU = "Съешь ещё этих мягких французских булок, да выпей чаю"
LAT = "naïve café — jamón"

PAGES = {
    # utf-8, said in the header
    "/utf8.html": ("utf-8", "header",
                   "<!doctype html><title>utf-8</title>"
                   "<style>body{background:#101418;color:#d8dee9}"
                   "h1{color:#7ee787}</style>"
                   "<h1>utf-8</h1><p>" + RU + "</p><p>" + LAT + "</p>"),
    # windows-1251, said in the header
    "/cp1251.html": ("cp1251", "header",
                     "<!doctype html><title>1251</title>"
                     "<style>body{background:#101418;color:#d8dee9}"
                     "h1{color:#79c0ff}</style>"
                     "<h1>windows-1251, from the header</h1><p>" + RU + "</p>"),
    # windows-1251, said only in a meta tag
    "/meta1251.html": ("cp1251", "meta",
                       "<!doctype html><meta charset=\"windows-1251\">"
                       "<title>meta 1251</title>"
                       "<style>body{background:#101418;color:#d8dee9}"
                       "h1{color:#ffa657}</style>"
                       "<h1>windows-1251, from a meta tag</h1><p>" + RU + "</p>"),
    # koi8-r, said in the header
    "/koi8.html": ("koi8-r", "header",
                   "<!doctype html><title>koi8</title>"
                   "<style>body{background:#101418;color:#d8dee9}"
                   "h1{color:#d2a8ff}</style>"
                   "<h1>koi8-r</h1><p>" + RU + "</p>"),
}

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)
for f in (SER, MON):
    try:
        os.unlink(f)
    except OSError:
        pass
subprocess.run(["cp", HOME + "/disk.img", OUT + "/disk.img"], check=True)


class Server(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path not in PAGES:
            self.send_error(404)
            return
        enc, where, text = PAGES[self.path]
        body = text.encode(enc, "replace")
        self.send_response(200)
        if where == "header":
            self.send_header("Content-Type", "text/html; charset=%s" % enc)
        else:
            self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.ThreadingTCPServer(("0.0.0.0", 8712), Server)
threading.Thread(target=srv.serve_forever, daemon=True).start()

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
        sys.exit("no WM")
    time.sleep(0.25)
print("wm up")
time.sleep(4)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(60):
    try:
        s.connect(MON)
        break
    except OSError:
        time.sleep(0.25)
s.settimeout(0.3)


def drain():
    try:
        while True:
            if not s.recv(65536):
                break
    except socket.timeout:
        pass


def cmd(c, settle=0.08):
    s.sendall((c + "\n").encode())
    time.sleep(settle)
    drain()


KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '\n': 'ret',
        '_': 'shift-minus', ':': 'shift-semicolon', '\t': 'tab'}


def keyname(ch):
    if ch in KEYS:
        return KEYS[ch]
    if 'A' <= ch <= 'Z':
        return 'shift-' + ch.lower()
    return ch


def typ(t, settle=0.05):
    for ch in t:
        cmd("sendkey " + keyname(ch), settle)


def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.2)


def go(url, wait=9):
    cmd("sendkey l", 0.5)
    for _ in range(90):
        cmd("sendkey backspace", 0.015)
    typ(url + "\n")
    time.sleep(wait)


base = "http://10.0.2.2:8712"
typ("web " + base + "/utf8.html\n")
time.sleep(12)
shot("01_utf8")

for name in ("cp1251", "meta1251", "koi8"):
    go(base + "/" + name + ".html")
    shot("02_" + name)

print("shots taken")
print("the sentence every page carries:")
print("   ", RU)

log = serial()
bad = [l for l in log.splitlines() if "PANIC" in l or "FAULT" in l]
print("panics/faults:", bad if bad else "(none)")

cmd("quit", 0.3)
time.sleep(1)
proc.kill()
srv.shutdown()
for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("shots in", OUT)
