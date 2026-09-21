#!/usr/bin/env python3
"""Run a console program on the OS and print what it said.

The serial line carries the console, so the answer comes back as text rather
than as a photograph of text -- which matters when the answer is a list of
checks that either passed or did not.
"""
import os, socket, subprocess, sys, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/nsrun"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"
CMD = sys.argv[1] if len(sys.argv) > 1 else "nstest"
WAIT = int(sys.argv[2]) if len(sys.argv) > 2 else 25

os.makedirs(OUT, exist_ok=True)
for f in (SER, MON):
    try:
        os.unlink(f)
    except OSError:
        pass
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
time.sleep(4)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(60):
    try:
        s.connect(MON)
        break
    except OSError:
        time.sleep(0.25)
s.settimeout(0.3)


def send(c, settle=0.08):
    s.sendall((c + "\n").encode())
    time.sleep(settle)
    try:
        while True:
            if not s.recv(65536):
                break
    except socket.timeout:
        pass


KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '\n': 'ret',
        '_': 'shift-minus', ':': 'shift-semicolon'}


def typ(t):
    for ch in t:
        n = KEYS.get(ch)
        if n is None:
            n = ('shift-' + ch.lower()) if 'A' <= ch <= 'Z' else ch
        send("sendkey " + n, 0.05)


mark = len(serial())
typ(CMD + "\n")
time.sleep(WAIT)

send("screendump %s/shot.ppm" % OUT, 1.0)
out = serial()[mark:]
send("quit", 0.3)
time.sleep(1)
proc.kill()

subprocess.run(["convert", OUT + "/shot.ppm", OUT + "/shot.png"], check=False)

print("--- what the program printed " + "-" * 40)
for line in out.splitlines():
    print(line)
print("-" * 68)
bad = [l for l in serial().splitlines() if "PANIC" in l or "FAULT" in l]
print("panics/faults:", bad if bad else "(none)")
print("screen in", OUT + "/shot.png")
