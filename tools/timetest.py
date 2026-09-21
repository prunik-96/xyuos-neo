#!/usr/bin/env python3
"""Check xyuOS's calendar against the host's, over a wide spread of dates.

A date conversion is exactly the kind of code that looks right and is wrong
for one month in four hundred years. So it is not read, it is run: the same
source, built for the host, against the host's own gmtime and mktime.
"""
import os, subprocess, sys, random, datetime

HOME = "/home/roman/xyuos-neo"
OUT = "/tmp/timecheck"
os.makedirs(OUT, exist_ok=True)

PROBE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(void) {
    long v;
    while (scanf("%ld", &v) == 1) {
        time_t t = (time_t)v;
        struct tm *g = gmtime(&t);
        if (g == NULL) { printf("NULL\n"); continue; }
        struct tm copy = *g;
        time_t back = mktime(&copy);
        char buf[64];
        strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", g);
        printf("%s wday=%d yday=%d back=%ld\n",
               buf, g->tm_wday, g->tm_yday, (long)back);
    }
    return 0;
}
'''

# The host's mktime reads local time; the probe must not, or every answer is
# off by a timezone. TZ=UTC settles it for both builds.
os.environ["TZ"] = "UTC"

open(OUT + "/probe.c", "w").write(PROBE)

for name, extra in (("host", []),
                    ("mine", ["-I", HOME + "/libc/include",
                              HOME + "/libc/src/timecal.c"])):
    r = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-o", OUT + "/" + name,
                        OUT + "/probe.c"] + extra,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("could not build the %s probe:\n%s" % (name, r.stderr))
    if r.stderr.strip():
        print("warnings building %s:\n%s" % (name, r.stderr.strip()))

# Dates chosen to land on the awkward parts: leap days, century years, the
# epoch itself, the ends of months, and a wide random spread besides.
stamps = []


def add(y, m, d, hh=0, mm=0, ss=0):
    try:
        dt = datetime.datetime(y, m, d, hh, mm, ss,
                               tzinfo=datetime.timezone.utc)
    except ValueError:
        return
    stamps.append(int(dt.timestamp()))


add(1970, 1, 1)
add(1970, 1, 1, 0, 0, 1)
add(1969, 12, 31, 23, 59, 59)
for y in (1900, 1904, 1996, 1999, 2000, 2001, 2004, 2020, 2024, 2025,
          2038, 2100, 2400):
    add(y, 1, 1)
    add(y, 2, 28, 23, 59, 59)
    add(y, 2, 29)                 # skipped by add() when it is not a leap year
    add(y, 3, 1)
    add(y, 12, 31, 23, 59, 59)
add(2038, 1, 19, 3, 14, 7)        # where a 32-bit time_t would stop

random.seed(11)
for _ in range(4000):
    stamps.append(random.randrange(-3000000000, 4000000000))
for _ in range(1000):
    stamps.append(random.randrange(0, 2000000000))

inp = "\n".join(str(v) for v in stamps) + "\n"

out = {}
for name in ("host", "mine"):
    r = subprocess.run([OUT + "/" + name], input=inp, capture_output=True,
                       text=True, timeout=120, env=dict(os.environ, TZ="UTC"))
    if r.returncode != 0:
        sys.exit("the %s probe exited %d\n%s" % (name, r.returncode, r.stderr))
    out[name] = r.stdout.splitlines()

if len(out["host"]) != len(out["mine"]):
    sys.exit("different numbers of answers: %d and %d"
             % (len(out["host"]), len(out["mine"])))

bad = []
for v, a, b in zip(stamps, out["host"], out["mine"]):
    if a != b:
        bad.append((v, a, b))

print("%d timestamps, %d agree with the host's calendar"
      % (len(stamps), len(stamps) - len(bad)))

if bad:
    print("\n%d differ:\n" % len(bad))
    for v, a, b in bad[:15]:
        print("   t=%d" % v)
        print("     host: %s" % a)
        print("     ours: %s" % b)
    sys.exit(1)

# And that the round trip really closes, which is the property the rest of
# the port leans on.
broke = 0
for v, line in zip(stamps, out["mine"]):
    back = int(line.rsplit("back=", 1)[1])
    if back != v:
        broke += 1
        if broke <= 5:
            print("round trip failed: %d came back as %d" % (v, back))
if broke:
    sys.exit("%d timestamps did not survive mktime(gmtime(t))" % broke)
print("every one survives mktime(gmtime(t)) unchanged")
