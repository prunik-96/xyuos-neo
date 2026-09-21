#!/usr/bin/env python3
"""Sign in, be remembered, sign out -- with the server as the judge.

A cookie that is stored but never sent looks exactly like a cookie that
works, from the inside. So the server here decides: it hands out a session on
sign-in, and every later page says plainly whether the request arrived with
it. It also sets a cookie for a path the browser must NOT send everywhere,
and one marked Secure that must not travel over plain http at all.
"""
import http.server, os, socket, socketserver, subprocess, sys, threading, time
from urllib.parse import parse_qs, urlparse

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/cookietest"
SER = OUT + "/serial.log"
MON = OUT + "/mon.sock"

SESSION = "s3cr3t-session-value"
seen = []


def page(title, body):
    return ("<!doctype html><title>%s</title>"
            "<style>body{background:#101418;color:#d8dee9}"
            "h1{color:#7ee787}</style>%s" % (title, body)).encode()


class Site(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def cookies(self):
        raw = self.headers.get("Cookie") or ""
        out = {}
        for bit in raw.split(";"):
            if "=" in bit:
                k, v = bit.split("=", 1)
                out[k.strip()] = v.strip()
        return out, raw

    def reply(self, body, extra=()):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        for h, v in extra:
            self.send_header(h, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        got, raw = self.cookies()
        seen.append((self.path, raw))

        if path == "/":
            self.reply(page("Sign in",
                            "<h1>sign in</h1>"
                            "<form action=\"/login\" method=\"get\">"
                            "<input type=\"text\" name=\"user\" size=\"16\" value=\"\">"
                            "<input type=\"submit\" value=\"Sign in\"></form>"))
        elif path == "/login":
            user = parse_qs(urlparse(self.path).query).get("user", [""])[0]
            self.reply(
                page("Signed in", "<h1>signed in as %s</h1>"
                                  "<p><a href=\"/who\">who am I</a></p>" % user),
                extra=[("Set-Cookie", "sid=%s; Path=/; Max-Age=3600" % SESSION),
                       ("Set-Cookie", "name=%s; Path=/; Max-Age=3600" % user),
                       # Only for /admin: must not turn up on /who.
                       ("Set-Cookie", "adminonly=yes; Path=/admin; Max-Age=3600"),
                       # https only: must not turn up here at all.
                       ("Set-Cookie", "sslonly=yes; Path=/; Secure; Max-Age=3600")])
        elif path == "/who":
            who = got.get("name")
            sid = got.get("sid")
            self.reply(page("Who",
                            "<h1>%s</h1><p>session: %s</p>"
                            "<p><a href=\"/logout\">sign out</a></p>"
                            % ("you are " + who if who else "nobody knows you",
                               "yes" if sid == SESSION else "no")))
        elif path == "/logout":
            self.reply(
                page("Signed out", "<h1>signed out</h1>"
                                   "<p><a href=\"/who\">who am I</a></p>"),
                extra=[("Set-Cookie", "sid=; Path=/; Max-Age=0"),
                       ("Set-Cookie", "name=; Path=/; Max-Age=0")])
        else:
            self.reply(page("?", "<h1>%s</h1>" % path))


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

socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.ThreadingTCPServer(("0.0.0.0", 8712), Site)
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


def go(url, wait=8):
    cmd("sendkey l", 0.5)
    for _ in range(90):
        cmd("sendkey backspace", 0.015)
    typ(url + "\n")
    time.sleep(wait)


base = "http://10.0.2.2:8712"
typ("web " + base + "/\n")
time.sleep(12)

cmd("sendkey tab", 0.6)              # into the name box
typ("roman")
cmd("sendkey ret", 6.0)              # sign in: the server sets the cookies
shot("01_signed_in")

go(base + "/who")                    # does it remember?
shot("02_who")

go(base + "/logout")                 # and can it be told to forget?
go(base + "/who")
shot("03_after_logout")

print("shots taken")
print("every request, and the Cookie header it carried:")
for path, raw in seen:
    print("   %-28s %s" % (path, raw if raw else "(none)"))

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

print("\n/home/cookies.txt, as the OS left it:")
dst = OUT + "/cookies.txt"
subprocess.run(["debugfs", "-R", "dump /home/cookies.txt " + dst,
                OUT + "/disk.img"], capture_output=True, text=True)
try:
    with open(dst, "rb") as f:
        for line in f.read().decode("utf-8", "replace").splitlines():
            print("   ", line.replace("\t", " | "))
except OSError:
    print("    (no such file)")
print("\nshots in", OUT)
