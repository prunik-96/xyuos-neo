#!/usr/bin/env python3
"""Whose five hundred milliseconds is it?

The same page, the same server, fetched twice: once by NetSurf through the
fetcher written for it, once by the browser this system already had. Both go
through the same kernel call. If both are slow the kernel is slow; if only one
is, the fault is in the program above it.

Guessing between those two from the source is how an afternoon disappears.
"""
import http.server, os, socket, socketserver, subprocess, sys, threading, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/nsblame"
PORT = 8716
os.makedirs(OUT, exist_ok=True)

NSHEET = 8
PAGE = "<!doctype html><title>a page</title>"
for i in range(NSHEET):
    PAGE += "<link rel='stylesheet' href='/s%d.css'>" % i
PAGE += "<h1>a page</h1>"
for i in range(1, 40):
    PAGE += "<p class='p%d'>paragraph %d.</p>" % (i % NSHEET, i)
SHEET = "\n".join(".p%d { color: #%02x2040; }" % (i, i * 9)
                  for i in range(NSHEET))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        b = (SHEET if self.path.endswith(".css") else PAGE).encode()
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


def typ(t):
    for ch in t:
        cmd("sendkey " + KEYS.get(ch, ch), 0.05)


URL = "http://10.0.2.2:%d/p" % PORT

for who, shot in (("web", "01_web"), ("netsurf", "02_netsurf")):
    typ("%s %s\n" % (who, URL))
    time.sleep(18)
    cmd("sendkey esc", 1.5)          # both close on escape
    time.sleep(2)
    typ("netlog\n")
    time.sleep(3)
    cmd("screendump %s/%s.ppm" % (OUT, shot), 1.2)
    typ("clear\n")
    time.sleep(0.5)

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
