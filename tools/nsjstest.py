#!/usr/bin/env python3
"""Does JavaScript actually run, and can it reach the page.

An engine that evaluates 2+2 is a calculator. What makes it a browser is the
bindings: whether a script can find an element, read it, change it, and have
the change appear on screen. So this page does all four, and each result is
written into the document -- if the text on screen says it worked, it worked,
because the only thing that could have written it is the script.
"""
import http.server, os, socket, socketserver, subprocess, sys, threading, time

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT = "/tmp/nsjstest"
PORT = 8719
os.makedirs(OUT, exist_ok=True)
for f in list(os.listdir(OUT)):
    if f.endswith(".png") or f.endswith(".ppm"):
        os.unlink(OUT + "/" + f)

PAGE = """<!doctype html>
<title>javascript</title>
<style>
 body { font-family: sans-serif; margin: 16px }
 .r { border-left: 4px solid #ccc; padding-left: 8px; margin: 4px 0 }
 .ok { border-color: #2a2; }
 .no { border-color: #c22; }
</style>
<h2>what the script managed</h2>
<div id="out">the script did not run at all</div>
<p id="target">this paragraph is here to be changed</p>
<script>
var lines = [];
function say(what, good) {
  lines.push('<div class="r ' + (good ? 'ok' : 'no') + '">' + what + '</div>');
}

/* 1. the engine itself */
try {
  var n = 0; for (var i = 1; i <= 100; i++) n += i;
  say('arithmetic: 1 to 100 adds to ' + n, n === 5050);
} catch (e) { say('arithmetic threw: ' + e, false); }

/* 2. can it see the document */
try {
  var t = document.title;
  say('document.title is "' + t + '"', t === 'javascript');
} catch (e) { say('document.title threw: ' + e, false); }

/* 3. can it find an element */
try {
  var p = document.getElementById('target');
  say('getElementById found ' + (p ? p.tagName : 'nothing'), !!p);
} catch (e) { say('getElementById threw: ' + e, false); }

/* 4. can it change one, and does the change reach the screen */
try {
  var p2 = document.getElementById('target');
  p2.textContent = 'THIS TEXT WAS WRITTEN BY THE SCRIPT';
  p2.style.color = '#a22';
  say('changed the paragraph below', true);
} catch (e) { say('changing it threw: ' + e, false); }

/* 5. can it make a new element */
try {
  var d = document.createElement('div');
  d.textContent = 'and this whole line was created by the script';
  d.className = 'r ok';
  document.body.appendChild(d);
  say('created and appended an element', true);
} catch (e) { say('createElement threw: ' + e, false); }

document.getElementById('out').innerHTML = lines.join('');
</script>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        b = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = Server(("0.0.0.0", PORT), Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()

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
        ':': 'shift-semicolon'}
for ch in "netsurf http://10.0.2.2:%d/p\n" % PORT:
    cmd("sendkey " + KEYS.get(ch, ch), 0.05)

time.sleep(15)
cmd("screendump %s/01_js.ppm" % OUT, 1.5)

log = serial()
bad = [l for l in log.splitlines()
       if "PANIC" in l or "FAULT" in l or "#PF" in l or "#GP" in l]
print("panics/faults:", "\n   ".join(bad) if bad else "(none)")

cmd("quit", 0.3)
time.sleep(1)
proc.kill()
srv.shutdown()

for f in sorted(os.listdir(OUT)):
    if f.endswith(".ppm"):
        subprocess.run(["convert", OUT + "/" + f, OUT + "/" + f[:-4] + ".png"],
                       check=False)
        os.unlink(OUT + "/" + f)
print("pictures in " + OUT)
