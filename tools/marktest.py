#!/usr/bin/env python3
"""Keep pages as bookmarks, list them, drop one -- then read the file itself.

The list on screen could be a list of anything. What settles it is the file
the OS wrote, so this shuts the machine down and reads it out of the disk
image at the end.
"""
import http.server, os, socket, socketserver, subprocess, sys, threading, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/marktest"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"
IMG = OUT + "/disk.img"

PAGES = {
    "/one.html": b"<!doctype html><title>The first page</title>"
                 b"<h1>one</h1><p>the first of two</p>",
    "/two.html": b"<!doctype html><title>Tom &amp; Jerry &lt;b&gt;</title>"
                 b"<h1>two</h1><p>a title with characters a page cannot"
                 b" hold plainly</p>",
    "/three.html": b"<!doctype html><title>The third page</title>"
                   b"<h1>three</h1><p>the odd one out</p>",
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
subprocess.run(["cp", HOME + "/disk.img", IMG], check=True)


class Pages(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        body = PAGES.get(self.path, b"<!doctype html><title>?</title><p>no</p>")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.TCPServer(("0.0.0.0", 8712), Pages)
threading.Thread(target=srv.serve_forever, daemon=True).start()

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host",
     "-cdrom", HOME + "/build/xyuos_neo.iso",
     "-serial", "file:" + SER, "-m", "512M", "-display", "none",
     "-drive", "file=%s,if=none,id=d0,format=raw" % IMG,
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
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.0)


base = "http://10.0.2.2:8712"
typ("web " + base + "/one.html\n")
time.sleep(10)


def go(url, wait=7):
    cmd("sendkey l", 0.5)
    for _ in range(90):
        cmd("sendkey backspace", 0.015)
    typ(url + "\n")
    time.sleep(wait)


cmd("sendkey b", 1.0)          # keep the first
shot("01_kept_one")

go(base + "/two.html")
cmd("sendkey b", 1.0)          # keep the second
go(base + "/three.html")
cmd("sendkey b", 1.0)          # and the third

cmd("sendkey m", 2.0)          # the list
shot("02_list")

# Back to the second and drop it: the same key that kept it.
go(base + "/two.html")
cmd("sendkey b", 1.0)
cmd("sendkey m", 2.0)
shot("03_after_dropping")

print("shots taken")
log = serial()
bad = [l for l in log.splitlines() if "PANIC" in l or "FAULT" in l]
print("panics/faults:", bad if bad else "(none)")

cmd("quit", 0.3)
time.sleep(2)
proc.kill()
srv.shutdown()
time.sleep(1)

for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)

print("\n/home/bookmarks.txt, as the OS left it:")
dst = OUT + "/bookmarks.txt"
subprocess.run(["debugfs", "-R", "dump /home/bookmarks.txt " + dst, IMG],
               capture_output=True, text=True)
try:
    with open(dst, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    for line in text.splitlines():
        print("   ", line.replace("\t", "  ->  "))
    print("\n   %d line(s)" % len(text.splitlines()))
except OSError:
    print("    (no such file)")
print("\nshots in", OUT)
