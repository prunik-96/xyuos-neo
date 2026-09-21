#!/usr/bin/env python3
"""Point the browser at a page served from the host, so the pictures are known
quantities and the whole path -- fetch, decode, fit, paint -- can be judged."""
import os, socket, subprocess, sys, time

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT  = "/tmp/localimg"
SER  = OUT + "/serial.log"
MON  = OUT + "/mon.sock"
os.makedirs(OUT, exist_ok=True)
for f in (SER, MON):
    try: os.unlink(f)
    except OSError: pass
subprocess.run(["cp", HOME + "/disk.img", OUT + "/disk.img"], check=True)

srv = subprocess.Popen(["python3", "-m", "http.server", "8000", "--bind", "0.0.0.0"],
                       cwd="/tmp/webroot",
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)

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
        proc.kill(); srv.kill(); sys.exit("no WM")
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
        ':': 'shift-semicolon', '_': 'shift-minus'}
def keyname(ch):
    if ch in KEYS: return KEYS[ch]
    if 'A' <= ch <= 'Z': return 'shift-' + ch.lower()
    return ch
def typ(t):
    for ch in t: cmd("sendkey " + keyname(ch), 0.05)
def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 0.9)

typ("web http://10.0.2.2:8000/\n")
time.sleep(18)
shot("01_top")
cmd("sendkey pgdn", 1.0); time.sleep(1.5); shot("02")
cmd("sendkey pgdn", 1.0); time.sleep(1.5); shot("03")
cmd("sendkey pgdn", 1.0); time.sleep(1.5); shot("04")

log = serial()
bad = [l for l in log.splitlines() if "PANIC" in l or "FAULT" in l]
print("panics/faults:", bad if bad else "(none)")

cmd("quit", 0.3); time.sleep(1); proc.kill(); srv.kill()
for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"], check=False)
        os.unlink(OUT + "/" + f)
subprocess.run("cp %s/*.png /mnt/c/PC_WORK/nettest/" % OUT, shell=True)
print("shots in", OUT)
