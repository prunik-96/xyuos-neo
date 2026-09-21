#!/usr/bin/env python3
"""Drive a form in the OS browser and report what the server was actually sent.

A screenshot shows a box with a word in it. It does not show whether pressing
Enter sent q=wombat or sent nothing at all, and that is the whole question a
form has to answer. So the server here writes down every request it gets, and
the test prints them next to the pictures.
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
OUT = "/tmp/formtest"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"
DOC = ("/mnt/c/Users/roman/AppData/Local/Temp/claude/"
       "C--PC-WORK/43986ff9-fa9f-4b19-8833-6417b40024cc/scratchpad")
PAGE = sys.argv[1] if len(sys.argv) > 1 else "formpage.html"

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


class Recorder(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=DOC, **kw)

    def reply(self, what):
        body = ("<!doctype html><title>got it</title>"
                "<h2>the server was sent</h2><pre>%s</pre>"
                % what.replace("&", "&amp;").replace("<", "&lt;"))
        b = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        ctype = self.headers.get("Content-Type") or "(none)"
        seen.append("POST %s  len=%s type=%s  body=%r" % (self.path, self.headers.get("Content-Length"), ctype, body))
        self.reply("POST " + self.path + "\n" + body)

    def do_GET(self):
        seen.append("GET  " + self.path)
        # Anything that is not the page itself is a form arriving. Answer it
        # with what it sent, so the browser has something to show.
        if not self.path.startswith("/" + PAGE):
            self.reply(self.path)
            return
        super().do_GET()


socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.TCPServer(("0.0.0.0", 8712), Recorder)
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
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.0)


# A local file is served from the scratchpad; anything else is an address
# of its own, so the same driver works on a live site.
if PAGE.endswith(".html"):
    typ("web http://10.0.2.2:8712/" + PAGE + "\n")
    time.sleep(12)
else:
    typ("web " + PAGE + "\n")
    time.sleep(70)
shot("01_loaded")

# Tab into the first field, clear what is there, and type something of ours.
cmd("sendkey tab", 0.4)
for _ in range(12):
    cmd("sendkey backspace", 0.05)
typ("wombat", 0.08)
shot("02_typed")

# Enter is the form's submit.
cmd("sendkey ret", 3.0)
shot("03_sent")

# And once more with Tab moving on to the next fields, to see focus travel.
print("shots taken")
print("the server was asked for:")
for p in seen:
    print("   ", p)

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
