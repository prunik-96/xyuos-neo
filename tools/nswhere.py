#!/usr/bin/env python3
"""Where a page's time actually goes.

The browser is timed from outside by the server, which sees when each request
arrived. The kernel is asked afterwards how long each of those requests took
inside it. The gap between the two is time the program spent doing something
other than asking, and that gap is the thing worth knowing before changing
anything else.
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
OUT = "/tmp/nswhere"
PORT = 8715
os.makedirs(OUT, exist_ok=True)

NSHEET = 12
PAGE = "<!doctype html><title>a page</title>"
for i in range(NSHEET):
    PAGE += "<link rel='stylesheet' href='/s%d.css'>" % i
PAGE += "<h1>a page</h1>"
for i in range(1, 121):
    PAGE += "<p class='p%d'>paragraph %d.</p>" % (i % NSHEET, i)
SHEET = "\n".join(".p%d { color: #%02x2040; }" % (i, i * 9)
                  for i in range(NSHEET))

hits = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        hits.append((self.path, time.time()))
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


typ("netsurf http://10.0.2.2:%d/p\n" % PORT)

deadline = time.time() + 90
while time.time() < deadline:
    time.sleep(0.5)
    if hits and time.time() - hits[-1][1] > 6:
        break

span = (hits[-1][1] - hits[0][1]) if len(hits) > 1 else 0
print("\nthe server saw %d requests over %.1fs" % (len(hits), span))
if len(hits) > 1:
    gaps = [(hits[i + 1][1] - hits[i][1]) * 1000 for i in range(len(hits) - 1)]
    gaps.sort()
    print("   gap between requests: shortest %.0fms, middle %.0fms, "
          "longest %.0fms" % (gaps[0], gaps[len(gaps) // 2], gaps[-1]))

# Close the browser and ask the kernel what it thinks those cost.
cmd("sendkey esc", 1.0)
time.sleep(2)
typ("netlog\n")
time.sleep(3)
cmd("screendump %s/log.ppm" % OUT, 1.0)

cmd("quit", 0.3)
time.sleep(1)
proc.kill()
srv.shutdown()

subprocess.run(["convert", OUT + "/log.ppm", OUT + "/log.png"], check=False)
print("the kernel's own view is in " + OUT + "/log.png")
