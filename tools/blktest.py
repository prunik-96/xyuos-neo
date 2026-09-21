#!/usr/bin/env python3
"""Which block device does the OS actually bind, and does the network card
change the answer? The RAM disk fallback is silent from inside the system --
everything works, nothing persists."""
import subprocess, time, os

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
BASE = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host",
        "-cdrom", HOME + "/build/xyuos_neo.iso", "-m", "512M", "-display", "none"]

cases = [
    ("virtio only", ["-drive", "file=%s/disk.img,if=none,id=d0,format=raw" % HOME,
                     "-device", "virtio-blk-pci,drive=d0"]),
    ("virtio + e1000", ["-drive", "file=%s/disk.img,if=none,id=d0,format=raw" % HOME,
                        "-device", "virtio-blk-pci,drive=d0",
                        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]),
    ("virtio + usb + e1000", ["-drive", "file=%s/disk.img,if=none,id=d0,format=raw" % HOME,
                              "-device", "virtio-blk-pci,drive=d0",
                              "-device", "qemu-xhci,id=xhci",
                              "-device", "usb-kbd,bus=xhci.0",
                              "-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]),
]

for name, extra in cases:
    log = "/tmp/blk_%s.log" % name.replace(" ", "_").replace("+", "")
    try: os.unlink(log)
    except OSError: pass
    p = subprocess.Popen(BASE + ["-serial", "file:" + log] + extra,
                         stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    t0 = time.time()
    while time.time() - t0 < 30:
        time.sleep(1)
        try:
            s = open(log).read()
        except OSError:
            continue
        if "vfs:" in s:
            break
    p.kill()
    s = ""
    try: s = open(log).read()
    except OSError: pass
    lines = [l for l in s.splitlines()
             if "blkdev" in l.lower() or "virtio" in l.lower() or "pci:" in l.lower()]
    print("%-22s %s" % (name, lines[:3] if lines else "(nothing)"))
