#!/usr/bin/env python3
"""Type several commands in the shell, photographing after each.

    python3 tools/seqrun.py 2G "sigtest:25" "ls:3"

Each step is COMMAND:SECONDS -- what to type, and how long to let it run
before the screen is photographed. A step with nothing before the colon just
presses Enter, which is how a message box left by a deliberate crash is got
out of the way before the next command. A step starting with @ names keys
instead of typing text: "@pgdn pgdn:2" presses Page Down twice. One starting
with ! is QEMU monitor commands, separated by ';': "!mouse_move 300 200:1"
moves the pointer.

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

SEQRUN_USBNET=1 puts the network on QEMU's USB RNDIS adapter instead of the
e1000: the path a phone's USB tethering takes on real hardware.

SEQRUN_AUDIO=/path/out.wav gives the machine an Intel HD Audio controller
with a line-out codec, and QEMU writes everything played on it to that WAV
file -- what the system actually sent to the speakers, to be checked.

SEQRUN_PUT="host:guest,host2:guest2" copies files onto the run's copy of the
disk first (never onto disk.img itself): test material that should not ship.

SEQRUN_TABLET=1 gives the machine QEMU's usb-tablet instead of the mouse:
an absolute pointer, so "!mouse_move X Y" puts it at screen pixel X, Y and a
click lands where it is aimed ("!mouse_button 1;mouse_button 0").

SEQRUN_NOHID=1 leaves out the keyboard and mouse on the controller's own
ports, and SEQRUN_USBX adds QEMU arguments (split at spaces) -- together they
put the keyboard behind a hub: SEQRUN_USBX="-device usb-hub,bus=xhci.0,port=1
-device usb-kbd,bus=xhci.0,port=1.1". Steps starting with ! can plug devices
in and out while it runs (drive_add / device_add / device_del).
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
TABLET = os.environ.get("SEQRUN_TABLET") == "1"
SCREEN = tuple(int(v) for v in os.environ.get("SEQRUN_SCREEN", "1920x1080").split("x"))
RAM = sys.argv[1] if len(sys.argv) > 1 else "2G"
STEPS = sys.argv[2:] or ["sigtest:25"]

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if not (KEEP and f == "stick.img"):
        os.unlink(OUT + "/" + f)
SER, MON, QMP = OUT + "/serial.log", OUT + "/mon.sock", OUT + "/qmp.sock"
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
    for pair in filter(None, os.environ.get("SEQRUN_PUT", "").split(",")):
        host, guest = pair.split(":", 1)
        subprocess.run(["debugfs", "-w", "-R", "write %s %s" % (host, guest),
                        OUT + "/disk.img"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
BOOT = [] if STICK else ["-cdrom", XYUOS + "/build/xyuos_neo.iso"]
# SEQRUN_USBNET=1: the network through QEMU's USB RNDIS adapter instead of the
# e1000 -- the path a tethered phone takes on real hardware.
NET = (["-nic", "none", "-netdev", "user,id=n0",
        "-device", "usb-net,bus=xhci.0,netdev=n0"]
       if os.environ.get("SEQRUN_USBNET") == "1" else [])
FIRMWARE = ["-bios", "/usr/share/qemu/OVMF.fd"] if UEFI else []
# SEQRUN_VGA=virtio gives the machine the virtio GPU instead of the standard VGA.
VGA = ["-vga", os.environ["SEQRUN_VGA"]] if os.environ.get("SEQRUN_VGA") else []
AUDIO_OUT = os.environ.get("SEQRUN_AUDIO")
AUDIO = (["-audiodev", "wav,id=snd0,path=" + AUDIO_OUT,
          "-device", "intel-hda", "-device", "hda-output,audiodev=snd0"]
         if AUDIO_OUT else [])

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host"] + BOOT + FIRMWARE +
    ["-serial", "file:" + SER, "-m", RAM, "-display", "none",
     "-smp", SMP] + DISK +
    ["-device", "qemu-xhci,id=xhci"] +
    (["-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0"] if STICK else []) +
    ([] if os.environ.get("SEQRUN_NOHID") == "1" else
     ["-device", "usb-kbd,bus=xhci.0",
      "-device", ("usb-tablet" if os.environ.get("SEQRUN_TABLET") == "1" else "usb-mouse") + ",bus=xhci.0"]) +
    os.environ.get("SEQRUN_USBX", "").split() +
    ["-monitor", "unix:%s,server,nowait" % MON, "-qmp", "unix:%s,server,nowait" % QMP] +
    NET + AUDIO + VGA,
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

# QMP as well: the only way to put an absolute pointer at a place (HMP's
# mouse_move only ever moves, and a tablet ignores moves).
import json
q = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(60):
    try:
        q.connect(QMP); break
    except OSError:
        time.sleep(0.25)
q.settimeout(0.02)       # answers come at once; waiting longer spoils double-clicks


def qmp(obj, settle=0.02):
    q.sendall((json.dumps(obj) + "\n").encode())
    time.sleep(settle)
    try:
        while True:
            r = q.recv(65536)
            if not r:
                break
            if b"error" in r and os.environ.get("SEQRUN_QMPDEBUG"):
                sys.stderr.write("qmp: " + r.decode("utf-8", "replace") + "\n")
    except socket.timeout:
        pass


qmp({"execute": "qmp_capabilities"})


def pointer_to(x, y):
    ax = int(x * 32767 / (SCREEN[0] - 1))
    ay = int(y * 32767 / (SCREEN[1] - 1))
    qmp({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}}]}})


def button(name, down):
    qmp({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": down, "button": name}}]}})


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
    elif line.startswith("!"):
        # QEMU monitor commands, separated by ';': "!mouse_move 200 100;
        # mouse_button 1; mouse_button 0:1" moves the pointer and clicks.
        # With the tablet, "click X Y", "rclick X Y" and "dclick X Y" click
        # at screen pixel X, Y (QEMU wants the tablet's 0..32767 scale).
        for c in line[1:].split(";"):
            c = c.strip()
            w = c.split()
            if TABLET and w and w[0] in ("mouse_move", "click", "rclick", "dclick", "move",
                                          "press", "release") and len(w) >= 3:
                pointer_to(int(w[1]), int(w[2]))
                time.sleep(0.1)
                if w[0] in ("click", "dclick"):
                    for _ in range(2 if w[0] == "dclick" else 1):
                        button("left", True)
                        button("left", False)
                elif w[0] == "rclick":
                    button("right", True)
                    button("right", False)
                elif w[0] == "press":
                    button("left", True)
                elif w[0] == "release":
                    button("left", False)
                time.sleep(0.1)
                continue
            cmd(c, 0.15)
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
