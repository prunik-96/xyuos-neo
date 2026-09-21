#!/usr/bin/env python3
"""Collect the files NetSurf expects to find beside itself.

Two kinds. The stylesheets are the important ones: default.css is the user
agent stylesheet, and in CSS everything a reader thinks of as "how a web page
looks by default" -- paragraphs as blocks, headings bold and larger, lists
indented -- comes from there and not from the page. Without it NetSurf lays
every page out as one run of inline text.

The other is Messages, NetSurf's own wording, which it ships as one file with
every language in it. Pulling English out is what its own build does with
utils/split-messages.pl; the same few lines are here so the port does not
need perl.
"""
import os, shutil, subprocess, sys

NS = "/home/roman/src/ns/netsurf-3.9/resources"
OUT = "/home/roman/xyuos-neo/third_party/nsxyuos/res"

if not os.path.isdir(NS):
    sys.exit("no NetSurf resources at " + NS)

os.makedirs(OUT, exist_ok=True)

# --- the stylesheets and the about: pages ---------------------------------
FILES = ["default.css", "quirks.css", "adblock.css", "internal.css",
         "netsurf.png", "favicon.png"]
LANG = ["welcome.html", "credits.html", "licence.html", "maps.html"]

taken = []
for f in FILES:
    src = os.path.join(NS, f)
    if os.path.exists(src):
        shutil.copy(src, os.path.join(OUT, f))
        taken.append(f)

for f in LANG:
    src = os.path.join(NS, "en", f)
    if os.path.exists(src):
        shutil.copy(src, os.path.join(OUT, f))
        taken.append(f)

# The little pictures beside links and in lists. NetSurf asks for these under
# resource:icons/..., so they keep their subdirectory -- and without them
# every one of them draws as the alt text, which is a question mark.
icons = os.path.join(NS, "icons")
if os.path.isdir(icons):
    os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)
    n = 0
    for f in sorted(os.listdir(icons)):
        src = os.path.join(icons, f)
        if os.path.isfile(src):
            shutil.copy(src, os.path.join(OUT, "icons", f))
            n += 1
    taken.append("icons/ (%d files)" % n)

# NetSurf asks for favicon.ico by that name; what it ships is a png. Under
# resource: the name is the whole address, so the copy has to answer to it.
if os.path.exists(os.path.join(OUT, "favicon.png")):
    shutil.copy(os.path.join(OUT, "favicon.png"), os.path.join(OUT, "favicon.ico"))
    taken.append("favicon.ico")

# --- the English half of the message catalogue -----------------------------
# Lines are "lang.platform.Key:Value". Anything for another language is not
# ours; "all" applies everywhere, and a platform-specific line for a platform
# that is not this one would be wrong to take.
KEEP_PLATFORM = ("all", "gtk", "ami")   # in that order of preference
msgs = {}
order = []

# Read as bytes: the file is meant to be UTF-8 throughout and one of the
# translations is not, so decoding the whole thing fails on a line this does
# not want. Only the English lines are decoded, and only they have to be
# valid.
bad_lines = 0
with open(os.path.join(NS, "FatMessages"), "rb") as f:
    raw_lines = f.read().split(b"\n")

for raw in raw_lines:
    if not raw.startswith(b"en."):
        continue
    try:
        line = raw.decode("utf-8").rstrip("\r")
    except UnicodeDecodeError:
        bad_lines += 1
        continue
    # Only the first colon separates: a value may contain more.
    head, sep, value = line.partition(":")
    if not sep:
        continue
    bits = head.split(".", 2)
    if len(bits) != 3:
        continue
    _lang, platform, key = bits
    if platform not in KEEP_PLATFORM:
        continue
    # A more specific platform does not override "all": this is not gtk, and
    # the "all" line is the one written to be true everywhere.
    if key in msgs and msgs[key][0] == "all":
        continue
    if key not in msgs:
        order.append(key)
    msgs[key] = (platform, value)

with open(os.path.join(OUT, "Messages"), "w", encoding="utf-8") as f:
    f.write("# NetSurf's own wording, English, taken out of FatMessages.\n")
    for key in order:
        f.write("%s:%s\n" % (key, msgs[key][1]))
taken.append("Messages (%d entries, %d lines skipped as not UTF-8)" % (len(order), bad_lines))

print("collected into %s:" % OUT)
for t in taken:
    print("   " + t)
