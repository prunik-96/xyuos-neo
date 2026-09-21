#!/bin/sh
# The link, spelled out, so its complaints can be read whole.
#
# Same two roots as the Python tools: the project is found from this script's
# own location, and the unpacked sources default to ~/src.
H=${XYUOS:-$(cd "$(dirname "$0")/.." && pwd)}
S=${XYUOS_SRC:-$HOME/src}

LD=$H/toolchain/cross/bin/x86_64-elf-ld
NSL=$H/third_party/netsurf

$LD -nostdlib -static -o $S/ns/link/netsurf.elf \
    -T $H/userland/link.ld \
    $H/build/crt0.o \
    $H/third_party/nsxyuos/libnsglue.a \
    $S/ns/link/libnscore.a \
    $NSL/libdom.a $NSL/libcss.a $NSL/libhubbub.a $NSL/libparserutils.a \
    $NSL/libwapcaplet.a $NSL/libnsutils.a $NSL/libnsbmp.a $NSL/libutf8proc.a \
    $H/third_party/shim/libnsshim.a \
    $H/build/libc.a \
    $H/third_party/nsxyuos/libnsglue.a $S/ns/link/libnscore.a \
    $NSL/libdom.a $NSL/libcss.a $NSL/libhubbub.a $NSL/libparserutils.a \
    $NSL/libwapcaplet.a $NSL/libnsutils.a $NSL/libnsbmp.a $NSL/libutf8proc.a \
    $H/third_party/shim/libnsshim.a $H/build/libc.a
