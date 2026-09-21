#!/bin/bash
set -e

export PREFIX="$HOME/xyuos-neo/toolchain/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

SRC="$HOME/xyuos-neo/toolchain/src"
cd "$SRC"

rm -rf build-gcc
mkdir -p build-gcc
cd build-gcc

../gcc-13.2.0/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c,c++ --without-headers

make -j"$(nproc)" all-gcc
make -j"$(nproc)" all-target-libgcc
make install-gcc
make install-target-libgcc

echo "=== DONE ==="
"$PREFIX/bin/$TARGET-gcc" --version
