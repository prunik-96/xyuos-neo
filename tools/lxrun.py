#!/usr/bin/env python3
"""Run the Lexbor test program on xyuOS and photograph what it prints."""
import os, socket, subprocess, sys, time

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
ELF = XYUOS_SRC + "/lxprog/lxtest"
OUT = XYUOS_SRC + "/lxrun"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)
for f in (SER, MON):
    try:
        os.unlink(f)
    except OSError:
        pass

if not os.path.exists(ELF):
    sys.exit("no binary -- run tools/lxprog.py first")

subprocess.run(["cp", HOME + "/disk.img", OUT + "/disk.img"], check=True)
subprocess.run(["debugfs", "-w", "-R", "rm /bin/lxtest", OUT + "/disk.img"],
               capture_output=True)
r = subprocess.run(["debugfs", "-w", "-R", "write %s /bin/lxtest" % ELF,
                    OUT + "/disk.img"], capture_output=True, text=True)
if "Allocated inode" not in r.stdout + r.stderr:
    sys.exit("could not put it on the disc:\n" + r.stdout + r.stderr)
print("lxtest: %.2f MB on the disc" % (os.path.getsize(ELF) / 1e6))

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


KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '\n': 'ret'}


def typ(t):
    for ch in t:
        cmd("sendkey " + KEYS.get(ch, ch), 0.05)


for arg, shot in (("tree", "01_tree"), ("enc", "02_encodings")):
    typ("lxtest %s\n" % arg)
    time.sleep(6)
    cmd("screendump %s/%s.ppm" % (OUT, shot), 1.2)
    typ("clear\n")
    time.sleep(0.6)

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
