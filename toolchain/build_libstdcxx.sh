#!/bin/bash
# libsupc++ for x86_64-elf: the part of the C++ runtime that has nothing to do
# with the operating system.
#
# What is already here and what is not, because the answer surprised me:
#
#   The stack unwinder IS present. It lives inside libgcc.a (unwind-dw2.o and
#   friends), not in a separate libgcc_eh.a -- that split only happens when
#   shared libraries are being built, which for a bare target they are not.
#
#   What is missing is libsupc++: __cxa_throw, __cxa_allocate_exception, the
#   personality routine __gxx_personality_v0, and the typeinfo machinery. That
#   is the C++ half of exceptions; libgcc only provides the machine half.
#
# --disable-hosted-libstdcxx is what makes this buildable at all without a
# hosted C library underneath. It builds the freestanding subset: libsupc++
# plus the headers that need no operating system. No iostreams, no std::string
# -- those want a hosted libc and are a separate question.
#
# This builds in its OWN tree and installs ONLY the libstdc++ artifacts. The
# compiler that is working today is not touched: `make install-gcc` is never
# run here, deliberately.
set -e

PREFIX="$HOME/xyuos-neo/toolchain/cross"
TARGET=x86_64-elf
SRC="$HOME/xyuos-neo/toolchain/src"
BUILD="$SRC/build-libstdcxx"

export PATH="$PREFIX/bin:$PATH"

# The target compiler has to be findable by its bare name: the libstdc++
# configure looks for x86_64-elf-gcc on PATH, not in the build tree.
command -v "$TARGET-gcc" >/dev/null || { echo "no $TARGET-gcc on PATH"; exit 1; }

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

"$SRC/gcc-13.2.0/configure" --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c,c++ --without-headers \
    --disable-hosted-libstdcxx --disable-libstdcxx-verbose \
    --disable-shared --disable-threads

# all-gcc and all-target-libgcc come along as prerequisites and are built
# again in this tree. That is the price of not disturbing the installed one.
make -j"$(nproc)" all-target-libstdc++-v3
make install-target-libstdc++-v3

echo "=== DONE ==="
ls -la "$PREFIX/lib/gcc/$TARGET"/*/libsupc++.a 2>/dev/null || \
    find "$PREFIX" -name 'libsupc++.a'
