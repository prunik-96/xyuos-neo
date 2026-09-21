#!/usr/bin/env python3
"""Three things NetSurf was getting wrong, checked against evidence.

A screenshot cannot tell you whether the server was asked for /search?q=x or
/searchq=x -- both produce a page. So the server here writes down the request
line it actually received, and the test prints it.

The same run times how long a page takes and scrolls it, because those were
the other two complaints and both are measurable rather than a matter of
opinion.
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
OUT = "/tmp/nscheck"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"
PORT = 8713

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

seen = []

# A page long enough to have somewhere to scroll to, with a form whose
# submission carries a query string.
LONG_PAGE = ("<!doctype html><title>a long page</title>"
             "<h1>top of the page</h1>"
             "<form action='/search' method='get'>"
             "<input name='q' value='wombat'>"
             "<input type='submit' value='Search'></form>")
for i in range(1, 121):
    LONG_PAGE += "<p>paragraph number %d, here to make the page tall.</p>" % i
LONG_PAGE += "<h1 id='end'>bottom of the page</h1>"


class Recorder(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        seen.append(self.path)
        if self.path.startswith("/search"):
            body = ("<!doctype html><title>the server was asked</title>"
                    "<h1>the server was asked for</h1><pre>%s</pre>"
                    % self.path.replace("&", "&amp;").replace("<", "&lt;"))
        else:
            body = LONG_PAGE
        b = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = Server(("0.0.0.0", PORT), Recorder)
threading.Thread(target=srv.serve_forever, daemon=True).start()
print("server on %d" % PORT)

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
        ':': 'shift-semicolon', '=': 'equal', '?': 'shift-slash',
        ',': 'comma', ';': 'semicolon', '&': 'shift-7', '_': 'shift-minus',
        '+': 'shift-equal', '%': 'shift-5', '#': 'shift-3'}


def keyname(ch):
    if ch in KEYS:
        return KEYS[ch]
    if 'A' <= ch <= 'Z':
        return 'shift-' + ch.lower()
    return ch


def typ(t):
    for ch in t:
        cmd("sendkey " + keyname(ch), 0.05)


def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.0)


URL = "http://10.0.2.2:%d/page" % PORT

# --- how long a page takes -------------------------------------------------
typ("netsurf %s\n" % URL)

start = time.time()
while not seen:
    if time.time() - start > 60:
        break
    time.sleep(0.05)
asked_at = time.time()

# Wait for the page to actually appear, not merely to be requested.
time.sleep(6)
shot("01_loaded")
print("\nrequest reached the server %.1fs after the command"
      % (asked_at - start))

# --- how hard it is working when nothing is happening ----------------------
#
# The window manager counts how many times each program hands it a finished
# frame, and prints the tally every sixtieth frame. On a page that has
# finished loading and is not being touched, a browser should hand over
# nothing at all: there is nothing new to look at. Anything else is work
# taken away from the next page.


def blits_over(seconds):
    """The app blit counts the window manager reports during this window."""
    mark = len(serial())
    time.sleep(seconds)
    tail = serial()[mark:]
    return [int(l.split("app blits=")[1].split()[0])
            for l in tail.splitlines() if "app blits=" in l]


cmd("sendkey meta_l-p", 0.5)            # profiling on
idle = blits_over(6)
print("frames handed over while idle:   %s" % (idle if idle else "none at all"))

# --- scrolling -------------------------------------------------------------
for _ in range(12):
    cmd("sendkey pgdn", 0.15)
time.sleep(1.5)
shot("02_scrolled_keys")

# Back to the top with the wheel, which is how a page is actually scrolled.
# Positive is away from the user, which scrolls up.
moving = []
mark = len(serial())
for _ in range(14):
    cmd("mouse_move 0 0 1", 0.10)
time.sleep(1.0)
tail = serial()[mark:]
moving = [int(l.split("app blits=")[1].split()[0])
          for l in tail.splitlines() if "app blits=" in l]
print("frames handed over while scrolling: %s"
      % (moving if moving else "none -- the wheel did nothing"))
shot("03_scrolled_wheel")
cmd("sendkey meta_l-p", 0.3)            # profiling off

# --- a query string --------------------------------------------------------
before = len(seen)
typ("l")
time.sleep(0.5)
# Clear the field and type a URL that carries a query.
for _ in range(80):
    cmd("sendkey backspace", 0.02)
typ("http://10.0.2.2:%d/search?q=wombat&n=2\n" % PORT)
time.sleep(6)
shot("04_query")

srv.shutdown()

print("\nwhat the server was actually asked for:")
for p in seen:
    print("   " + p)

hits = [p for p in seen if p.startswith("/search")]
ok = any("?q=wombat" in p for p in hits)
print("\nquery string arrived intact: %s" % ("yes" if ok else "NO"))
if hits and not ok:
    print("   got %r -- the '?' is still being lost" % hits[-1])

log = serial()
bad = [l for l in log.splitlines()
       if "PANIC" in l or "FAULT" in l or "#PF" in l or "#GP" in l]
print("panics/faults:", "\n   ".join(bad) if bad else "(none)")

cmd("quit", 0.3)
time.sleep(1)
proc.kill()

for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("pictures in " + OUT)
