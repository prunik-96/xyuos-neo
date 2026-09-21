#!/usr/bin/env python3
"""Serve the same page compressed, and check the browser reads it.

A page that decompresses wrong does not look broken -- it looks empty, or it
looks like the last page. So the page carries a long, distinctive line, the
screenshot has to show it, and the server reports how many bytes it actually
put on the wire against how many the page really is.
"""
import gzip as gziplib
import http.server, os, socket, socketserver, subprocess, sys, threading, time, zlib

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT = "/tmp/gziptest"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"

MARK = "the quick brown fox jumps over the lazy dog"
PLAIN = ("<!doctype html><title>Compressed</title>"
         "<style>body{background:#101418;color:#d8dee9}h1{color:#7ee787}</style>"
         "<h1>gzip</h1><p>" + MARK + "</p>"
         # Something worth compressing, so the saving is real and not noise.
         + ("<p>filler filler filler filler filler filler filler</p>" * 400)
         + "<h2>end of the page</h2>").encode()

sent = []


class Server(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        accept = self.headers.get("Accept-Encoding") or ""
        if self.path.startswith("/gzip"):
            body = gziplib.compress(PLAIN)
            enc = "gzip"
        elif self.path.startswith("/deflate"):
            body = zlib.compress(PLAIN)
            enc = "deflate"
        else:
            body = PLAIN
            enc = None
        sent.append((self.path, accept, len(body), len(PLAIN), enc))

        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        if enc:
            self.send_header("Content-Encoding", enc)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


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


def go(url, wait=10):
    cmd("sendkey l", 0.5)
    for _ in range(90):
        cmd("sendkey backspace", 0.015)
    typ(url + "\n")
    time.sleep(wait)


base = "http://10.0.2.2:8712"
typ("web " + base + "/gzip.html\n")
time.sleep(14)
shot("01_gzip")

go(base + "/deflate.html")
shot("02_deflate")

go(base + "/plain.html")
shot("03_plain")

print("shots taken")
print("what the server put on the wire:")
for path, accept, out_n, real_n, enc in sent:
    print("   %-16s encoding=%-8s %7d bytes for a %d byte page"
          % (path, enc or "none", out_n, real_n))
    print("       the browser asked for: %s" % accept)

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
