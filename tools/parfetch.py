#!/usr/bin/env python3
"""Load one page and say what the parallel fetcher actually did.

Different from nsreal.py in the two ways that matter here: it watches the
serial port for a SECOND boot banner, which is the only sign a triple fault
leaves behind, and it reads the kernel's request log to see whether requests
actually overlapped in time -- which is the whole point of the change and is
not visible from a screenshot.
"""
import os, re, socket, subprocess, sys, time

XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = "/tmp/parfetch"
URL = sys.argv[1] if len(sys.argv) > 1 else "https://github.com/"
WAIT = int(sys.argv[2]) if len(sys.argv) > 2 else 70

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith((".png", ".ppm")):
        os.unlink(OUT + "/" + f)

SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"
for f in (SER, MON):
    try:
        os.unlink(f)
    except OSError:
        pass
subprocess.run(["cp", XYUOS + "/disk.img", OUT + "/disk.img"], check=True)

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host",
     "-cdrom", XYUOS + "/build/xyuos_neo.iso",
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


BANNER = "wm: double-buffer"
t0 = time.time()
while BANNER not in serial():
    if time.time() - t0 > 120 or proc.poll() is not None:
        proc.kill()
        sys.exit("the window manager never came up")
    time.sleep(0.25)
print("up")
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
        ':': 'shift-semicolon', '_': 'shift-minus', '?': 'shift-slash',
        '=': 'equal', '&': 'shift-7', ',': 'comma', ';': 'semicolon'}


def typ(t):
    for ch in t:
        name = KEYS.get(ch)
        if name is None:
            name = ('shift-' + ch.lower()) if 'A' <= ch <= 'Z' else ch
        cmd("sendkey " + name, 0.05)


typ("netsurf %s\n" % URL)

# Watch for a reset while it loads: a triple fault prints nothing at all, and
# the only trace is the kernel's banner appearing a second time.
died_at = None
for i in range(WAIT):
    time.sleep(1)
    if serial().count(BANNER) > 1:
        died_at = i + 1
        break

cmd("screendump %s/page.ppm" % OUT, 1.5)

if died_at is not None:
    print("\n*** the machine RESET after about %d s -- triple fault ***" % died_at)
else:
    print("\nno reset")
    cmd("sendkey esc", 1.5)
    time.sleep(2)
    typ("netlog\n")
    time.sleep(3)
    cmd("screendump %s/log.ppm" % OUT, 1.5)

log = serial()
bad = [l for l in log.splitlines()
       if any(k in l for k in ("PANIC", "FAULT", "#PF", "#GP", "HEAP:"))]
print("panics/faults:", "\n   ".join(bad) if bad else "(none)")
print("boots seen:", log.count(BANNER))

cmd("quit", 0.3)
time.sleep(1)
proc.kill()

for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("pictures in " + OUT)
