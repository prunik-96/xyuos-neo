#!/usr/bin/env python3
"""Two tabs holding the same shape of page, filled in differently.

Two pages parsed into the same shape get the same element numbering, and the
values carried across a rebuild are matched by that number -- so this is the
case where one tab's answers can quietly turn up in the other. Each tab is
made to send its form at the end, and the server says which answers arrived.
"""
import http.server, os, socket, socketserver, subprocess, sys, threading, time

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/tabformtest"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"


def page(name, colour):
    return ("<!doctype html><title>%s</title>"
            "<style>body{background:#101418;color:#d8dee9}"
            "h1{color:%s}</style>"
            "<h1>%s</h1>"
            "<form action=\"/sent-%s\" method=\"get\">"
            "<input type=\"text\" name=\"q\" size=\"24\" value=\"\">"
            "<input type=\"checkbox\" name=\"tick\"> tick"
            "<input type=\"submit\" value=\"Send\">"
            "</form>" % (name, colour, name, name.lower())).encode()


PAGES = {"/alpha.html": page("Alpha", "#7ee787"),
         "/beta.html": page("Beta", "#79c0ff")}

os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)
for f in (SER, MON):
    try:
        os.unlink(f)
    except OSError:
        pass
subprocess.run(["cp", HOME + "/disk.img", OUT + "/disk.img"], check=True)

asked = []


class Pages(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        asked.append(self.path)
        if self.path in PAGES:
            body = PAGES[self.path]
        else:
            body = ("<!doctype html><title>got it</title><h1>got it</h1>"
                    "<pre>%s</pre>" % self.path).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.TCPServer(("0.0.0.0", 8712), Pages)
threading.Thread(target=srv.serve_forever, daemon=True).start()

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


def typ(t, settle=0.06):
    for ch in t:
        cmd("sendkey " + keyname(ch), settle)


def shot(n):
    cmd("screendump %s/%s.ppm" % (OUT, n), 1.2)


def let_go():
    """Escape lets go of a focused field -- and, with nothing focused, it
    closes the browser. So it is pressed once, exactly where something is
    known to be holding the keyboard, and nowhere else. Switching tabs lets
    go by itself, so no Escape is needed after one."""
    cmd("sendkey esc", 0.3)


def go(url, wait=8):
    cmd("sendkey l", 0.5)
    for _ in range(90):
        cmd("sendkey backspace", 0.015)
    typ(url + "\n")
    time.sleep(wait)


base = "http://10.0.2.2:8712"

typ("web " + base + "/alpha.html\n")
time.sleep(10)
cmd("sendkey tab", 0.5)            # into alpha's text box
typ("aaa")
shot("01_alpha_filled")
let_go()

cmd("sendkey t", 2.5)              # a second tab, nothing focused in it
go(base + "/beta.html")
cmd("sendkey tab", 0.5)
typ("bbb")
shot("02_beta_filled")
let_go()

# Back and forth. If a tab were wearing the other one's answers, this is
# where it would show.
cmd("sendkey 1", 3.0)
shot("03_back_on_alpha")
cmd("sendkey 2", 3.0)
shot("04_back_on_beta")
cmd("sendkey 1", 3.0)

cmd("sendkey tab", 0.6)            # into alpha's box again
cmd("sendkey ret", 5.0)            # and send it
shot("05_alpha_sent")

cmd("sendkey 2", 3.0)
cmd("sendkey tab", 0.6)
cmd("sendkey ret", 5.0)
shot("06_beta_sent")

print("shots taken")
print("the server was asked for:")
for a in asked:
    print("   ", a)

log = serial()
bad = [l for l in log.splitlines() if "PANIC" in l or "FAULT" in l]
print("panics/faults:", bad if bad else "(none)")

cmd("quit", 0.3)
time.sleep(1)
proc.kill()
srv.shutdown()
for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("shots in", OUT)
