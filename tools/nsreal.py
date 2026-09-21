#!/usr/bin/env python3
"""Try real sites, and say where it stops.

Every test so far has pointed at a server on this machine, reached by its
address. A real site needs two more things: a name looked up in DNS, and for
almost all of them, TLS. Neither has ever been exercised through this
browser, so either could have been broken the whole time.

Each address is tried in its own run of the browser, and the kernel's own
request log is read afterwards -- that separates "the name did not resolve"
from "the connection failed" from "it arrived and did not render".
"""
import os, socket, subprocess, sys, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/nsreal"
os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)

TRY = sys.argv[1:] or [
    "http://example.com/",          # plain, a name to resolve
    "https://example.com/",         # the same over TLS
]

SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"
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
        sys.exit("the window manager never came up")
    time.sleep(0.25)
print("the system is up\n")
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


# First, does the network work at all outside the browser: resolve a name
# and fetch a page with the system's own tools.
typ("ping example.com\n")
time.sleep(8)
cmd("screendump %s/00_ping.ppm" % OUT, 1.2)
typ("clear\n")
time.sleep(0.5)

for i, url in enumerate(TRY):
    print("trying %s" % url)
    typ("netsurf %s\n" % url)
    time.sleep(60)
    cmd("screendump %s/%02d_page.ppm" % (OUT, i + 1), 1.5)
    cmd("sendkey esc", 1.5)
    time.sleep(2)
    typ("netlog\n")
    time.sleep(3)
    cmd("screendump %s/%02d_log.ppm" % (OUT, i + 1), 1.5)
    typ("clear\n")
    time.sleep(0.5)

log = serial()
bad = [l for l in log.splitlines()
       if "PANIC" in l or "FAULT" in l or "#PF" in l or "#GP" in l]
print("\npanics/faults:", "\n   ".join(bad) if bad else "(none)")

print("\nwhat the kernel said about the network:")
for l in log.splitlines():
    low = l.lower()
    if any(k in low for k in ("tls:", "dns", "net:", "nic:", "tcp")):
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
