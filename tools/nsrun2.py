#!/usr/bin/env python3
"""Put NetSurf on the disc and start it, then say what happened.

This is the first time the port runs rather than merely builds. It boots the
system in QEMU, types the program's name at the shell, and takes pictures.
What it is looking for first is not a rendered page -- it is whether the
program gets as far as opening a window without faulting, which is where a
port of this size usually stops.
"""
import os, socket, subprocess, sys, time

HOME = "/home/roman/xyuos-neo"
ELF = "/home/roman/xyuos-neo/third_party/nsxyuos/netsurf.elf"
OUT = "/tmp/nsrun"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"

URL = sys.argv[1] if len(sys.argv) > 1 else ""

os.makedirs(OUT, exist_ok=True)
for f in (SER, MON):
    try:
        os.unlink(f)
    except OSError:
        pass

if not os.path.exists(ELF):
    sys.exit("no binary; run tools/nslink.py first")

subprocess.run(["cp", HOME + "/disk.img", OUT + "/disk.img"], check=True)

# On to the disc, next to everything else in /bin.
r = subprocess.run(["debugfs", "-w", "-R", "rm /bin/netsurf", OUT + "/disk.img"],
                   capture_output=True, text=True)
r = subprocess.run(["debugfs", "-w", "-R", "write %s /bin/netsurf" % ELF,
                    OUT + "/disk.img"], capture_output=True, text=True)
if "Allocated inode" not in r.stdout + r.stderr:
    sys.exit("could not write the binary to the disc:\n" + r.stdout + r.stderr)
print("netsurf: %d bytes on the disc" % os.path.getsize(ELF))

# Its own files beside it: the user agent stylesheet above all, without which
# no page has block layout at all.
RES = HOME + "/third_party/nsxyuos/res"
subprocess.run(["debugfs", "-w", "-R", "mkdir /lib/netsurf", OUT + "/disk.img"],
               capture_output=True, text=True)
n = 0
for f in sorted(os.listdir(RES)):
    src = os.path.join(RES, f)
    if not os.path.isfile(src):
        continue
    subprocess.run(["debugfs", "-w", "-R", "rm /lib/netsurf/" + f,
                    OUT + "/disk.img"], capture_output=True, text=True)
    w = subprocess.run(["debugfs", "-w", "-R",
                        "write %s /lib/netsurf/%s" % (src, f),
                        OUT + "/disk.img"], capture_output=True, text=True)
    if "Allocated inode" in w.stdout + w.stderr:
        n += 1
print("resources: %d files in /lib/netsurf" % n)

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
        ':': 'shift-semicolon', '_': 'shift-minus', '?': 'shift-slash',
        '=': 'equal', '+': 'shift-equal', ',': 'comma', ';': 'semicolon',
        "'": 'apostrophe', '"': 'shift-apostrophe', '(': 'shift-9',
        ')': 'shift-0', '&': 'shift-7', '%': 'shift-5', '!': 'shift-1',
        '#': 'shift-3', '~': 'shift-grave_accent', '*': 'shift-8'}


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


typ("netsurf %s\n" % URL if URL else "netsurf\n")
time.sleep(10)
shot("01_start")

time.sleep(15)
shot("02_later")

log = serial()
bad = [l for l in log.splitlines()
       if "PANIC" in l or "FAULT" in l or "#PF" in l or "#GP" in l]
print("\npanics/faults:", "\n   ".join(bad) if bad else "(none)")

for l in log.splitlines():
    low = l.lower()
    if "netsurf" in low or "tls:" in low or "fetch" in low:
        print("   " + l)

cmd("quit", 0.3)
time.sleep(1)
proc.kill()

for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("\npictures in " + OUT)
