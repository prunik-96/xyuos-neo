#!/usr/bin/env python3
"""Exercise the chrome: tabs, the buttons, and whether they do what they say.

A screenshot of a toolbar proves it was drawn. It does not prove the back
button goes back. So this clicks things and photographs the result at each
step, and the pictures are the record.
"""
import http.server, os, socket, socketserver, subprocess, sys, threading, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/nschrome"
PORT = 8720
os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)


def page(n):
    return ("<!doctype html><title>page %d</title>"
            "<style>body{font-family:sans-serif;margin:24px}"
            "h1{color:#%s}</style>"
            "<h1>this is page %d</h1>"
            "<p><a href='/p%d'>go to page %d</a></p>"
            "<p>and here is some text so the page is not empty.</p>"
            % (n, ["a33", "2a6", "36a", "a63"][n % 4], n, n + 1, n + 1)).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        try:
            n = int(self.path.lstrip("/p") or "1")
        except ValueError:
            n = 1
        b = page(n)
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
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
        ':': 'shift-semicolon', '[': 'bracket_left', ']': 'bracket_right'}


def typ(t):
    for ch in t:
        cmd("sendkey " + KEYS.get(ch, ch), 0.05)


def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.2)


def home():
    for _ in range(24):
        cmd("mouse_move -100 -100", 0.02)


def click(x, y):
    home()
    dx, dy = x, y
    while dx > 0 or dy > 0:
        sx, sy = min(dx, 100), min(dy, 100)
        cmd("mouse_move %d %d" % (sx, sy), 0.02)
        dx -= sx
        dy -= sy
    time.sleep(0.3)
    cmd("mouse_button 1", 0.15)
    cmd("mouse_button 0", 0.3)


typ("netsurf http://10.0.2.2:%d/p1\n" % PORT)
time.sleep(10)
shot("01_page1")

# Follow the link on the page, so there is history to go back through.
click(120, 200)
time.sleep(5)
shot("02_page2")

# The back button is the first in the toolbar.
click(28, 47 + 24)
time.sleep(4)
shot("03_after_back")

# A second tab. The plus is at the end of the tab strip and guessing its
# pixel is how the last attempt missed; "t" is bound to the same action and
# needs no arithmetic.
typ("t")
time.sleep(8)
shot("04_two_tabs")

# And a third, then switch back to the first with the bracket keys.
typ("t")
time.sleep(8)
typ("[")
time.sleep(2)
shot("05_three_tabs")

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
