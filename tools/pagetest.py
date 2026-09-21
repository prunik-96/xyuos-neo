#!/usr/bin/env python3
"""Serve one page to the OS and photograph it: the top, then further down."""
import http.server, os, socket, socketserver, subprocess, sys, threading, time

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT  = "/tmp/pagetest"
SER  = OUT + "/serial.log"
MON  = OUT + "/mon.sock"
DOC  = ("/mnt/c/Users/roman/AppData/Local/Temp/claude/"
        "C--PC-WORK/43986ff9-fa9f-4b19-8833-6417b40024cc/scratchpad")
PAGE = sys.argv[1] if len(sys.argv) > 1 else "csspage.html"

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)
for f in (SER, MON):
    try: os.unlink(f)
    except OSError: pass
subprocess.run(["cp", HOME + "/disk.img", OUT + "/disk.img"], check=True)

class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=DOC, **kw)

socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.TCPServer(("0.0.0.0", 8712), Quiet)
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
        with open(SER, "rb") as f: return f.read().decode("utf-8", "replace")
    except OSError: return ""

t0 = time.time()
while "wm: double-buffer" not in serial():
    if time.time() - t0 > 120 or proc.poll() is not None:
        proc.kill(); sys.exit("no WM")
    time.sleep(0.25)
print("wm up"); time.sleep(4)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(60):
    try: s.connect(MON); break
    except OSError: time.sleep(0.25)
s.settimeout(0.3)

def drain():
    try:
        while True:
            if not s.recv(65536): break
    except socket.timeout: pass

def cmd(c, settle=0.08):
    s.sendall((c + "\n").encode()); time.sleep(settle); drain()

KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '\n': 'ret',
        '_': 'shift-minus', ':': 'shift-semicolon'}
def keyname(ch):
    if ch in KEYS: return KEYS[ch]
    if 'A' <= ch <= 'Z': return 'shift-' + ch.lower()
    return ch
def typ(t):
    for ch in t: cmd("sendkey " + keyname(ch), 0.05)
def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.0)

# A local file is served from the scratchpad; anything else is taken as an
# address of its own.
if PAGE.endswith(".html"):
    typ("web http://10.0.2.2:8712/" + PAGE + "\n")
    wait = 12
else:
    typ("web " + PAGE + "\n")
    wait = 70

time.sleep(wait)
shot("01_top")
cmd("sendkey n", 0.8)
shot("01_panel")
cmd("sendkey n", 0.5)
for page in range(2, 7):
    for _ in range(5):
        cmd("sendkey pgdn", 0.12)
    time.sleep(1.2)
    shot("%02d_more" % page)
print("shots taken")

log = serial()
bad = [l for l in log.splitlines() if "PANIC" in l or "FAULT" in l]
print("panics/faults:", bad if bad else "(none)")

cmd("quit", 0.3); time.sleep(1); proc.kill(); srv.shutdown()
for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
subprocess.run("cp %s/*.png /mnt/c/PC_WORK/nettest/ 2>/dev/null" % OUT, shell=True)
print("shots in", OUT)
