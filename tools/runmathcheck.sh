#!/bin/bash
# Compile our math.c with the host compiler, rename every symbol it exports so
# it can sit beside glibc's, and compare the two.
set -e
cd /tmp && rm -rf mathchk && mkdir mathchk && cd mathchk
X=~/xyuos-neo

gcc -c -O2 -I $X/libc/include $X/libc/src/math.c -o ours.o

SYMS="sqrt exp log log2 log10 log1p expm1 sin cos tan asin acos atan
      sinh cosh tanh asinh acosh atanh floor ceil trunc round fabs
      pow atan2 fmod ldexp frexp modf copysign"
ARGS=""
for s in $SYMS; do ARGS="$ARGS --redefine-sym $s=my_$s"; done
objcopy $ARGS ours.o ours_renamed.o

gcc -O2 -o mathcheck /mnt/c/Users/roman/AppData/Local/Temp/claude/C--PC-WORK/43986ff9-fa9f-4b19-8833-6417b40024cc/scratchpad/mathcheck.c ours_renamed.o -lm
./mathcheck
