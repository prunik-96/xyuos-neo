#!/usr/bin/env python3
"""Run the Python demo inside the OS and read what it printed."""
import os, socket, subprocess, sys, time

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT  = "/tmp/pytest"
SER  = OUT + "/serial.log"
MON  = OUT + "/mon.sock"
os.makedirs(OUT, exist_ok=True)
for f in (SER, MON):
    try: os.unlink(f)
    except OSError: pass
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
        ':': 'shift-semicolon', '_': 'shift-minus', '(': 'shift-9',
        ')': 'shift-0', '"': 'shift-apostrophe', "'": 'apostrophe',
        '+': 'shift-equal', '=': 'equal', ',': 'comma', '*': 'shift-8'}
def keyname(ch):
    if ch in KEYS: return KEYS[ch]
    if 'A' <= ch <= 'Z': return 'shift-' + ch.lower()
    return ch
def typ(t):
    for ch in t: cmd("sendkey " + keyname(ch), 0.05)
def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 0.9)

cmds = sys.argv[1:] or ["python /lib/python/demo.py"]
for i, c in enumerate(cmds):
    print("  $", c)
    typ(c + "\n")
    time.sleep(20)
    shot("%02d" % (i + 1))

log = serial()
bad = [l for l in log.splitlines() if "PANIC" in l or "FAULT" in l]
print("panics/faults:", bad if bad else "(none)")

cmd("quit", 0.3); time.sleep(1); proc.kill()
for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"], check=False)
        os.unlink(OUT + "/" + f)
subprocess.run("cp %s/*.png /mnt/c/PC_WORK/nettest/" % OUT, shell=True)
print("shots in", OUT)
