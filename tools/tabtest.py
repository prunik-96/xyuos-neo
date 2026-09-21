#!/usr/bin/env python3
"""Three tabs, switched between -- and a server that counts what it was asked.

A screenshot shows three names in a strip. What it does not show is whether
coming back to a tab quietly fetched the whole page again, which is the only
thing that makes tabs worth having. So the server counts, and the count is
printed at the end.
"""
import http.server, os, socket, socketserver, subprocess, sys, threading, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/tabtest"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"

PAGES = {
    "/one.html": b"<!doctype html><title>Page One</title>"
                 b"<style>body{background:#101418;color:#d8dee9}"
                 b"h1{color:#7ee787}</style>"
                 b"<h1>ONE</h1><p>the first tab</p>",
    "/two.html": b"<!doctype html><title>Page Two</title>"
                 b"<style>body{background:#101418;color:#d8dee9}"
                 b"h1{color:#79c0ff}</style>"
                 b"<h1>TWO</h1><p>the second tab</p>",
    "/three.html": b"<!doctype html><title>Page Three</title>"
                   b"<style>body{background:#101418;color:#d8dee9}"
                   b"h1{color:#ffa657}</style>"
                   b"<h1>THREE</h1><p>the third tab</p>",
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

asked = []


class Pages(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        asked.append(self.path)
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


def go(url, wait=8):
    cmd("sendkey l", 0.5)
    for _ in range(90):
        cmd("sendkey backspace", 0.015)
    typ(url + "\n")
    time.sleep(wait)


base = "http://10.0.2.2:8712"
typ("web " + base + "/one.html\n")
time.sleep(10)

cmd("sendkey t", 2.0)          # a second tab
go(base + "/two.html")
cmd("sendkey t", 2.0)          # a third
go(base + "/three.html")
shot("01_three_tabs")

after_loading = len(asked)
print("requests while loading the three:", after_loading)

cmd("sendkey 1", 3.0)          # back to the first
shot("02_back_to_one")
cmd("sendkey 2", 3.0)          # and the second
shot("03_back_to_two")
cmd("sendkey 3", 3.0)
after_switching = len(asked)

cmd("sendkey 2", 2.5)
cmd("sendkey w", 3.0)          # close the middle one
shot("04_after_closing")
after_closing = len(asked)

print("shots taken")
print("requests after switching around:", after_switching,
      "(+%d)" % (after_switching - after_loading))
print("requests after closing one:     ", after_closing,
      "(+%d)" % (after_closing - after_switching))
print("everything the server was asked for:")
for a in asked:
    print("   ", a)

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
