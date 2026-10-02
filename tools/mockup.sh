#!/bin/bash
# Build the mock-up renderer and paint the three variants into ~/dbg.
cd "$(dirname "$0")/.." || exit 1
gcc -O2 -w -Ilibrast/include -o /tmp/mockup tools/mockup.c \
    kernel/gfx/stb_truetype.c kernel/gfx/kmath.c kernel/wm/cursor.c librast/src/*.c -lm || exit 1
mkdir -p ~/dbg
/tmp/mockup /tmp/mock || exit 1
for v in 0 1 2 3; do
    python3 ~/ppm2png.py /tmp/mock$v.ppm ~/dbg/mock$v.png
done
ls -la ~/dbg/mock*.png
