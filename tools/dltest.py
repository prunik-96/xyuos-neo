#!/usr/bin/env python3
"""Download files in the OS browser, then look inside the disk image.

A screenshot of a status line saying "saved" is not evidence that anything was
saved. So this boots the OS, fetches three files, shuts it down, and then
reads the filesystem the OS wrote to -- comparing the bytes on disk with the
bytes the server sent.

The three cover the three ways a file gets a name and a reason to be kept:
the server names it in a header, the address is all there is to go on, and
the thing is readable so it is shown until somebody presses `s`.
"""
import hashlib, http.server, os, socket, socketserver, subprocess, sys
import threading, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/dltest"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"
IMG = OUT + "/disk.img"

# Two files: one the server names itself, one named only by its address.
NAMED = bytes(range(256)) * 12          # 3072 bytes, every byte value
PLAIN = b"# a small text file\nwith two lines\n"

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)
for f in (SER, MON):
    try:
        os.unlink(f)
    except OSError:
        pass
subprocess.run(["cp", HOME + "/disk.img", IMG], check=True)


class Files(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/thing"):
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition",
                             'attachment; filename="report v2.bin"')
            self.send_header("Content-Length", str(len(NAMED)))
            self.end_headers()
            self.wfile.write(NAMED)
        elif self.path.startswith("/notes/readme.txt"):
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(PLAIN)))
            self.end_headers()
            self.wfile.write(PLAIN)
        elif self.path.startswith("/data/blob.dat"):
            # No disposition: the name has to come out of the address.
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(NAMED)))
            self.end_headers()
            self.wfile.write(NAMED)
        else:
            body = (b"<!doctype html><title>files</title><h2>files</h2>"
                    b"<p><a href='/data/blob.dat'>blob</a></p>")
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)


socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.TCPServer(("0.0.0.0", 8712), Files)
threading.Thread(target=srv.serve_forever, daemon=True).start()

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host",
     "-cdrom", HOME + "/build/xyuos_neo.iso",
     "-serial", "file:" + SER, "-m", "512M", "-display", "none",
     "-drive", "file=%s,if=none,id=d0,format=raw" % IMG,
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
print("wm up")
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
        '_': 'shift-minus', ':': 'shift-semicolon', '\t': 'tab'}


def keyname(ch):
    if ch in KEYS:
        return KEYS[ch]
    if 'A' <= ch <= 'Z':
        return 'shift-' + ch.lower()
    return ch


def typ(t, settle=0.05):
    for ch in t:
        cmd("sendkey " + keyname(ch), settle)


def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.0)


# The OS boots to a shell, so the browser has to be started before there is
# an address bar to type into.
base = "http://10.0.2.2:8712"
typ("web " + base + "/\n")
time.sleep(10)


def go(url, wait=9):
    cmd("sendkey l", 0.5)                # focus the address bar
    for _ in range(90):
        cmd("sendkey backspace", 0.015)
    typ(url + "\n")
    time.sleep(wait)


go(base + "/thing")
shot("01_named")

# text/plain is something to read, so this one is SHOWN, not kept -- and then
# kept anyway, because `s` saves whatever is on screen.
go(base + "/notes/readme.txt")
shot("02_text_shown")
cmd("sendkey s", 2.5)
shot("03_text_saved")

go(base + "/data/blob.dat")
shot("04_from_url")

print("shots taken")
log = serial()
bad = [l for l in log.splitlines() if "PANIC" in l or "FAULT" in l]
print("panics/faults:", bad if bad else "(none)")

cmd("quit", 0.3)
time.sleep(2)
proc.kill()
srv.shutdown()
time.sleep(1)

for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)

# --- and now the part a picture cannot tell you -----------------------------
print("\nwhat is in /home/downloads on the disk the OS wrote to:")
ls = subprocess.run(["debugfs", "-R", "ls -l /home/downloads", IMG],
                    capture_output=True, text=True)
print(ls.stdout.strip() or "(nothing, or no such directory)")
if ls.stderr.strip():
    print("debugfs:", ls.stderr.strip().splitlines()[-1])


def grab(name):
    dst = OUT + "/got_" + name.replace("/", "_")
    subprocess.run(["debugfs", "-R", "dump /home/downloads/%s %s" % (name, dst),
                    IMG], capture_output=True, text=True)
    try:
        with open(dst, "rb") as f:
            return f.read()
    except OSError:
        return None


def check(name, want):
    got = grab(name)
    if got is None:
        print("  %-20s MISSING" % name)
        return
    ok = (got == want)
    print("  %-20s %d bytes, sha %s  %s"
          % (name, len(got), hashlib.sha256(got).hexdigest()[:16],
             "same as sent" if ok else "DIFFERENT (sent %d bytes)" % len(want)))


print("\nbytes on disk against bytes sent:")
check("report_v2.bin", NAMED)   # named by the server
check("blob.dat", NAMED)        # named by its address
check("readme.txt", PLAIN)      # shown first, then saved by hand
print("\nshots in", OUT)
