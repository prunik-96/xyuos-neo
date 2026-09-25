#!/usr/bin/env python3
"""Run a program in the shell and photograph what it said.

Used for memtest, where the answer is a few lines of numbers and the point is
to see them rather than to infer them from whether anything crashed. Takes the
amount of RAM as an argument because the interesting question -- how much can
one program get -- is only interesting on a machine with enough to give.

    python3 tools/memrun.py "memtest" 1G
"""
import os, socket, subprocess, sys, time

XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = "/tmp/memrun"
CMD = sys.argv[1] if len(sys.argv) > 1 else "memtest"
RAM = sys.argv[2] if len(sys.argv) > 2 else "1G"
WAIT = int(sys.argv[3]) if len(sys.argv) > 3 else 60

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith((".png", ".ppm")):
        os.unlink(OUT + "/" + f)
SER, MON = OUT + "/serial.log", OUT + "/mon.sock"
for f in (SER, MON):
    try:
        os.unlink(f)
    except OSError:
        pass
subprocess.run(["cp", XYUOS + "/disk.img", OUT + "/disk.img"], check=True)

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host",
     "-cdrom", XYUOS + "/build/xyuos_neo.iso",
     "-serial", "file:" + SER, "-m", RAM, "-display", "none",
     "-drive", "file=%s/disk.img,if=none,id=d0,format=raw" % OUT,
     "-device", "virtio-blk-pci,drive=d0",
     "-device", "qemu-xhci,id=xhci",
     "-device", "usb-kbd,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0",
     "-monitor", "unix:%s,server,nowait" % MON],
    stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def serial():
    try:
        with open(SER, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


B = "wm: double-buffer"
t0 = time.time()
while B not in serial():
    if time.time() - t0 > 120 or proc.poll() is not None:
        proc.kill()
        sys.exit("never came up")
    time.sleep(0.25)
time.sleep(4)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(60):
    try:
        s.connect(MON); break
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
        ':': 'shift-semicolon', '_': 'shift-minus', '=': 'equal'}


def typ(t):
    for ch in t:
        n = KEYS.get(ch) or (('shift-' + ch.lower()) if 'A' <= ch <= 'Z' else ch)
        cmd("sendkey " + n, 0.05)


typ(CMD + "\n")
time.sleep(WAIT)
cmd("screendump %s/out.ppm" % OUT, 1.5)

log = serial()
bad = [l for l in log.splitlines()
       if any(k in l for k in ("PANIC", "FAULT", "#PF", "#GP", "HEAP:",
                               "faulted"))]
print("panics/faults:", "\n   ".join(bad) if bad else "(none)")
print("boots:", log.count(B))

cmd("quit", 0.3)
time.sleep(1)
proc.kill()
for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("picture in " + OUT)
