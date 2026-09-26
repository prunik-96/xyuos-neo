#!/usr/bin/env python3
"""Does closing the browser take the machine with it?

Load a page, let it settle, press escape, and then watch the serial port for
a second boot banner -- the only thing a triple fault leaves behind.
"""
import os, socket, subprocess, sys, time

XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = "/tmp/exittest"
URL = sys.argv[1] if len(sys.argv) > 1 else "https://example.com/"
NOESC = os.environ.get("NOESC") == "1"
SETTLE = int(sys.argv[2]) if len(sys.argv) > 2 else 25

os.makedirs(OUT, exist_ok=True)
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
     "-serial", "file:" + SER, "-m", "512M", "-display", "none",
     # QEMU gives one core unless told otherwise; XYUOS_SMP says how many.
     "-smp", os.environ.get("XYUOS_SMP", "1"),
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


typ("netsurf %s\n" % URL)
print("loading for %d s" % SETTLE)
time.sleep(SETTLE)
cmd("screendump %s/before.ppm" % OUT, 1.2)

print("pressing escape" if not NOESC else "NOT pressing escape")

if not NOESC: cmd("sendkey esc", 0.5)

died = None
for i in range(25):
    time.sleep(1)
    if serial().count(B) > 1:
        died = i + 1
        break

if died:
    print("*** RESET %d s after escape ***" % died)
else:
    print("survived the escape")
cmd("screendump %s/after.ppm" % OUT, 1.2)

log = serial()
bad = [l for l in log.splitlines()
       if any(k in l for k in ("PANIC", "FAULT", "#PF", "#GP", "HEAP:"))]
print("panics:", "\n   ".join(bad) if bad else "(none)")
print("boots:", log.count(B))

cmd("quit", 0.3)
time.sleep(1)
proc.kill()
for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("pictures in " + OUT)
