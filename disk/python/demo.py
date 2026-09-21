# A tour of what works, and a check that it does.
#
# Run it with:  python /lib/python/demo.py

import sys
import math
import json
import random
import re
import time
import os


def show(label, value):
    print("  %-22s %s" % (label, value))


print("Python on xyuOS Neo")
print("  version", sys.version)
print("  platform", sys.platform)
print("  path", sys.path)

print("\nnumbers")
show("1/3", 1 / 3)
show("2 ** 100", 2 ** 100)
show("large factorial", math.factorial(20) if hasattr(math, "factorial") else "-")
show("math.pi", math.pi)
show("math.sqrt(2)", math.sqrt(2))
show("math.sin(math.pi/6)", math.sin(math.pi / 6))
show("math.log(math.e)", math.log(math.e))
show("math.gamma(5)", math.gamma(5.0))
show("math.erf(1)", math.erf(1.0))

print("\nsequences and comprehensions")
squares = [x * x for x in range(10)]
show("squares", squares)
show("sum", sum(squares))
show("sorted, reversed", sorted(squares, reverse=True)[:5])
show("dict", {k: k * k for k in range(5)})
show("set", sorted({x % 5 for x in range(20)}))

print("\ntext")
show("upper", "привет".upper() if hasattr(str, "upper") else "-")
show("split", "a,b,c".split(","))
show("join", "-".join(["x", "y", "z"]))
show("format", "{} and {}".format(1, 2))
show("f-string", f"two plus two is {2 + 2}")

print("\nregular expressions")
m = re.search(r"(\d+)-(\d+)", "range 10-25 here")
show("search", m.group(1) + " to " + m.group(2) if m else "no match")

print("\njson")
doc = json.dumps({"name": "xyuOS", "version": 1, "list": [1, 2, 3]})
show("dumps", doc)
show("loads", json.loads(doc)["list"])

print("\nclasses and exceptions")


class Counter:
    def __init__(self):
        self.n = 0

    def add(self, k=1):
        self.n += k
        return self

    def __repr__(self):
        return "Counter(%d)" % self.n


show("chained calls", Counter().add().add(5).add(10))

try:
    1 / 0
except ZeroDivisionError as e:
    show("caught", repr(e))


def gen(n):
    for i in range(n):
        yield i * i


show("generator", list(gen(6)))

print("\nrandom")
random.seed(42)
show("randrange", [random.randrange(100) for _ in range(5)])

print("\nfiles")
with open("/tmp/py_demo.txt", "w") as f:
    f.write("written from Python\nsecond line\n")
with open("/tmp/py_demo.txt") as f:
    show("read back", repr(f.read()))
show("listdir /", sorted(os.listdir("/"))[:6])

print("\ntime")
t0 = time.ticks_ms()
total = 0
for i in range(200000):
    total += i
show("200k additions", "%d ms" % time.ticks_diff(time.ticks_ms(), t0))
show("total", total)

print("\nall of the above ran")
