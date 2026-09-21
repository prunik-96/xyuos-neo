#!/usr/bin/env python3
"""Open a graphical program, switch theme, and look at the pixels along the
window's edges -- the report is "dark lines down the sides of every window
after a theme change", which is a question about specific columns."""
import os, socket, subprocess, sys, time

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT  = "/tmp/edge"
SER  = OUT + "/serial.log"
MON  = OUT + "/mon.sock"
os.makedirs(OUT, exist_ok=True)
for f in (SER, MON):
    try: os.unlink(f)
    except OSError: pass

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host",
     "-cdrom", HOME + "/build/xyuos_neo.iso",
     "-serial", "file:" + SER, "-m", "512M", "-display", "none",
     "-drive", "file=/tmp/edge/disk.img,if=none,id=d0,format=raw,cache=writethrough",
     "-device", "virtio-blk-pci,drive=d0",
     "-device", "qemu-xhci,id=xhci",
     "-device", "usb-kbd,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0",
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

KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '\n': 'ret'}
def typ(t):
    for ch in t: cmd("sendkey " + KEYS.get(ch, ch), 0.05)

def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.0)

typ("view /pics/photo.jpg\n")
time.sleep(6)
shot("01_dark")

cmd("sendkey meta_l-t", 1.0)          # cycle the theme
time.sleep(3)
shot("02_light")

cmd("sendkey meta_l-t", 1.0)
time.sleep(3)
shot("03_dark_again")

cmd("quit", 0.3); time.sleep(1); proc.kill()

# --- read the frames and report the columns -------------------------------
def load(path):
    d = open(path, "rb").read()
    # P6\n<w> <h>\n255\n
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i+1].isspace(): i += 1
        j = i
        while not d[j:j+1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i:]

def px(img, w, x, y):
    o = (y * w + x) * 3
    return img[o], img[o+1], img[o+2]

for name in ("01_dark", "02_light", "03_dark_again"):
    p = "%s/%s.ppm" % (OUT, name)
    if not os.path.exists(p): continue
    w, h, img = load(p)
    print("\n=== %s (%dx%d) ===" % (name, w, h))
    y = 300                                  # well inside the window body
    row = [px(img, w, x, y) for x in range(0, 80)]
    print("  row y=%d, x=0..79:" % y)
    run_start, run_col = 0, row[0]
    for x in range(1, 80):
        if row[x] != run_col:
            print("     x %3d..%3d  #%02x%02x%02x" % (run_start, x - 1, *run_col))
            run_start, run_col = x, row[x]
    print("     x %3d..%3d  #%02x%02x%02x" % (run_start, 79, *run_col))
    subprocess.run(["convert", p, "%s/%s.png" % (OUT, name)], check=False)

subprocess.run("cp %s/*.png /mnt/c/PC_WORK/nettest/" % OUT, shell=True)
print("\nshots in", OUT)
