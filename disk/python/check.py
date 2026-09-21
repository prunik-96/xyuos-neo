# Does the arithmetic agree with what it should be, and does the clock move?
import math
import time

print("math")
cases = [
    ("sqrt(2)",     math.sqrt(2),          1.4142135623730951),
    ("pi",          math.pi,               3.141592653589793),
    ("e",           math.e,                2.718281828459045),
    ("sin(pi/6)",   math.sin(math.pi / 6), 0.49999999999999994),
    ("cos(0)",      math.cos(0.0),         1.0),
    ("log(e)",      math.log(math.e),      1.0),
    ("exp(1)",      math.exp(1.0),         2.718281828459045),
    ("pow(2,0.5)",  math.pow(2, 0.5),      1.4142135623730951),
    ("atan2(1,1)",  math.atan2(1, 1),      0.7853981633974483),
    ("gamma(5)",    math.gamma(5.0),       24.0),
    ("erf(1)",      math.erf(1.0),         0.8427007929497149),
    ("floor(-2.5)", math.floor(-2.5),      -3),
    ("fmod(10,3)",  math.fmod(10, 3),      1.0),
]
bad = 0
for name, got, want in cases:
    ok = (got == want) or (want != 0 and abs(got - want) / abs(want) < 1e-12)
    if not ok:
        bad += 1
    print("  %-12s %-22r %s" % (name, got, "ok" if ok else "WRONG, want %r" % want))
print("  %s" % ("all correct" if not bad else "%d wrong" % bad))

print("\nbig integers")
print("  2**64      ", 2 ** 64)
print("  factorial20", math.factorial(20))
print("  17**30     ", 17 ** 30)

print("\nclock")
t0 = time.ticks_ms()
n = 0
for i in range(300000):
    n += i * 2
t1 = time.ticks_ms()
print("  300k multiply-add took %d ms" % time.ticks_diff(t1, t0))
print("  ticks_ms now           ", t1)
time.sleep_ms(250)
print("  after sleeping 250 ms  ", time.ticks_ms(),
      "(moved %d)" % time.ticks_diff(time.ticks_ms(), t1))
