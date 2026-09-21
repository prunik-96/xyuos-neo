#!/usr/bin/env python3
"""Measure the redraw change instead of describing it.

Two builds of the same program: one that redraws only when something has
changed, and one that redraws every time round the loop, which is what this
frontend did until now. Both load the same page, sit still, and the window
manager's own profiler counts the frames each hands over.

The count is the whole point. A browser sitting on a finished page should
hand over nothing; every frame it hands over is time not spent on the next
page, on an emulated processor where software-rendering a page is not cheap.
"""
import http.server, os, re, socket, socketserver, subprocess, sys, threading, time

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT = "/tmp/nsab"
PORT = 8714
os.makedirs(OUT, exist_ok=True)

# A page with a dozen stylesheets. Subresources are the point: the kernel
# runs one fetch at a time, so they arrive in a queue, and the time from the
# first request to the last is exactly what "the page took twenty seconds"
# means. A single-file page would finish before any of this mattered.
NSHEET = 12
PAGE = "<!doctype html><title>a page</title>"
for i in range(NSHEET):
    PAGE += "<link rel='stylesheet' href='/s%d.css'>" % i
PAGE += "<h1>a page</h1>"
for i in range(1, 121):
    PAGE += "<p class='p%d'>paragraph number %d, here to make it tall.</p>" \
            % (i % NSHEET, i)

SHEET = "\n".join(".p%d { color: #%02x%02x40; margin-left: %dpx; }"
                  % (i, i * 9, 90 - i, i) for i in range(NSHEET))

hits = []           # (path, seconds since the run started)
t_start = [0.0]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        hits.append((self.path, time.time()))
        if self.path.endswith(".css"):
            b = SHEET.encode()
            ctype = "text/css"
        else:
            b = PAGE.encode()
            ctype = "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = Server(("0.0.0.0", PORT), Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()

MAIN = HOME + "/third_party/nsxyuos/xy_main.c"
original = open(MAIN).read()

VARIANTS = [
    ("only when something changed", None),
    ("every time round the loop",
     ("        if (xy_dirty) {\n            xy_dirty = false;",
      "        if (1) {   /* the old behaviour, for comparison */\n"
      "            xy_dirty = false;")),
]

results = {}

try:
    for label, sub in VARIANTS:
        src = original
        if sub is not None:
            assert sub[0] in src, "xy_main.c is not the shape expected"
            src = src.replace(sub[0], sub[1], 1)
        open(MAIN, "w").write(src)

        for step in ("tools/nsglue.py", "tools/nslink.py"):
            r = subprocess.run(["python3", HOME + "/" + step], cwd=HOME,
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit("%s failed for %r:\n%s" % (step, label, r.stdout + r.stderr))

        elf = OUT + "/netsurf.elf"
        subprocess.run([HOME + "/toolchain/cross/bin/x86_64-elf-strip",
                        "-o", elf, XYUOS_SRC + "/ns/link/netsurf.elf"], check=True)

        img = OUT + "/disk.img"
        subprocess.run(["cp", HOME + "/disk.img", img], check=True)
        subprocess.run(["debugfs", "-w", "-R", "rm /bin/netsurf", img],
                       capture_output=True)
        subprocess.run(["debugfs", "-w", "-R", "write %s /bin/netsurf" % elf,
                        img], capture_output=True)

        ser = OUT + "/serial.log"
        mon = OUT + "/mon.sock"
        for f in (ser, mon):
            try:
                os.unlink(f)
            except OSError:
                pass

        proc = subprocess.Popen(
            ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host",
             "-cdrom", HOME + "/build/xyuos_neo.iso",
             "-serial", "file:" + ser, "-m", "512M", "-display", "none",
             "-drive", "file=%s,if=none,id=d0,format=raw" % img,
             "-device", "virtio-blk-pci,drive=d0",
             "-device", "qemu-xhci,id=xhci",
             "-device", "usb-kbd,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0",
             "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
             "-monitor", "unix:%s,server,nowait" % mon],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

        def serial():
            try:
                with open(ser, "rb") as f:
                    return f.read().decode("utf-8", "replace")
            except OSError:
                return ""

        t0 = time.time()
        while "wm: double-buffer" not in serial():
            if time.time() - t0 > 120 or proc.poll() is not None:
                proc.kill()
                sys.exit("the window manager never came up")
            time.sleep(0.25)
        time.sleep(4)

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(60):
            try:
                s.connect(mon)
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

        KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus',
                '\n': 'ret', ':': 'shift-semicolon'}
        del hits[:]
        for ch in "netsurf http://10.0.2.2:%d/p\n" % PORT:
            cmd("sendkey " + KEYS.get(ch, ch), 0.05)

        # Wait until the requests stop arriving rather than for a fixed time:
        # a fixed wait would measure the wait.
        deadline = time.time() + 90
        while time.time() < deadline:
            time.sleep(0.5)
            if hits and time.time() - hits[-1][1] > 6:
                break

        css = [h for h in hits if h[0].endswith(".css")]
        if len(hits) < 2:
            print("%-32s nothing arrived" % label)
            results[label] = None
        else:
            span = hits[-1][1] - hits[0][1]
            results[label] = (span, len(hits), len(css))
            print("%-32s %5.1fs from the first request to the last "
                  "(%d requests, %d of them stylesheets)"
                  % (label, span, len(hits), len(css)))

        cmd("quit", 0.3)
        time.sleep(1)
        proc.kill()

finally:
    open(MAIN, "w").write(original)
    srv.shutdown()
    # Leave the tree with the real build, not the comparison one.
    subprocess.run(["python3", HOME + "/tools/nsglue.py"], cwd=HOME,
                   capture_output=True)
    subprocess.run(["python3", HOME + "/tools/nslink.py"], cwd=HOME,
                   capture_output=True)
    print("\nxy_main.c restored and rebuilt")
