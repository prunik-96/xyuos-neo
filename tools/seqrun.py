#!/usr/bin/env python3
"""Type several commands in the shell, photographing after each.

    python3 tools/seqrun.py 2G "sigtest:25" "ls:3"

Each step is COMMAND:SECONDS -- what to type, and how long to let it run
before the screen is photographed. A step with nothing before the colon just
presses Enter, which is how a message box left by a deliberate crash is got
out of the way before the next command. A step starting with @ names keys
instead of typing text: "@pgdn pgdn:2" presses Page Down twice.

The pictures land in /tmp/seqrun as step0.png, step1.png, ... and, if
SEQRUN_COPY names a directory, are copied there as well.

SEQRUN_SMP sets the number of cores. QEMU gives ONE unless told otherwise,
which is worth knowing: every test run without it is a single-core test.

SEQRUN_RAMDISK=1 leaves out the virtio disk, so the system runs from the copy
of the image GRUB loads into memory.

SEQRUN_STICK=1 boots the way real hardware does: the ISO written to a USB
stick, no CD and no virtio, so the disk is the partition on the stick. The
stick is a copy of the ISO in /tmp/seqrun; SEQRUN_KEEP=1 keeps the one from
the last run instead, to see that what was written survived. SEQRUN_UEFI=1
boots with OVMF instead of SeaBIOS, as the real machine does.
"""
import os, shutil, socket, subprocess, sys, time

XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = "/tmp/seqrun"
COPY = os.environ.get("SEQRUN_COPY")
SMP = os.environ.get("SEQRUN_SMP", "1")
RAMDISK = os.environ.get("SEQRUN_RAMDISK") == "1"
STICK = os.environ.get("SEQRUN_STICK") == "1"
KEEP = os.environ.get("SEQRUN_KEEP") == "1"
UEFI = os.environ.get("SEQRUN_UEFI") == "1"
RAM = sys.argv[1] if len(sys.argv) > 1 else "2G"
STEPS = sys.argv[2:] or ["sigtest:25"]

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if not (KEEP and f == "stick.img"):
        os.unlink(OUT + "/" + f)
SER, MON = OUT + "/serial.log", OUT + "/mon.sock"
DISK = []
if STICK:
    if not (KEEP and os.path.exists(OUT + "/stick.img")):
        subprocess.run(["cp", XYUOS + "/build/xyuos_neo.iso", OUT + "/stick.img"],
                       check=True)
    DISK = ["-drive", "file=%s/stick.img,if=none,id=stick,format=raw" % OUT]
elif not RAMDISK:
    subprocess.run(["cp", XYUOS + "/disk.img", OUT + "/disk.img"], check=True)
    DISK = ["-drive", "file=%s/disk.img,if=none,id=d0,format=raw" % OUT,
            "-device", "virtio-blk-pci,drive=d0"]
BOOT = [] if STICK else ["-cdrom", XYUOS + "/build/xyuos_neo.iso"]
FIRMWARE = ["-bios", "/usr/share/qemu/OVMF.fd"] if UEFI else []

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host"] + BOOT + FIRMWARE +
    ["-serial", "file:" + SER, "-m", RAM, "-display", "none",
     "-smp", SMP] + DISK +
    ["-device", "qemu-xhci,id=xhci"] +
    (["-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0"] if STICK else []) +
    ["-device", "usb-kbd,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0",
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


# QEMU names keys, not characters, and a name it does not know is dropped
# without a word -- so every character a step may contain needs its key here.
KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '\n': 'ret',
        ':': 'shift-semicolon', '_': 'shift-minus', '=': 'equal',
        '+': 'shift-equal', '*': 'shift-8', ',': 'comma',
        '|': 'shift-backslash', '>': 'shift-dot', '<': 'shift-comma',
        '"': 'shift-apostrophe', "'": 'apostrophe', '&': 'shift-7'}


def typ(t):
    for ch in t:
        n = KEYS.get(ch) or (('shift-' + ch.lower()) if 'A' <= ch <= 'Z' else ch)
        cmd("sendkey " + n, 0.05)


for i, step in enumerate(STEPS):
    line, _, wait = step.rpartition(":")
    if line.startswith("@"):
        # Keys by QEMU's names, pressed and not typed: "@pgdn pgdn:2" pages
        # down twice, with no Enter after.
        for k in line[1:].split():
            cmd("sendkey " + k, 0.15)
    else:
        typ(line + "\n")
    time.sleep(float(wait))
    cmd("screendump %s/step%d.ppm" % (OUT, i), 1.5)

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
        if COPY:
            os.makedirs(COPY, exist_ok=True)
            shutil.copy(OUT + "/" + f[:-4] + ".png", COPY)
print("pictures in " + OUT + (" and " + COPY if COPY else ""))
