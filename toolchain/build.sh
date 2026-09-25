#!/bin/bash
set -e

# Where this script lives, taken before anything below changes directory.
HERE="$(cd "$(dirname "$0")" && pwd)"

export PREFIX="$HOME/xyuos-neo/toolchain/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

SRC="$HOME/xyuos-neo/toolchain/src"
mkdir -p "$SRC" "$PREFIX"
cd "$SRC"

BINUTILS_VER=2.42
GCC_VER=13.2.0

if [ ! -f "binutils-$BINUTILS_VER.tar.xz" ]; then
    echo "=== Downloading binutils-$BINUTILS_VER ==="
    wget -q "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz"
fi
if [ ! -f "gcc-$GCC_VER.tar.xz" ]; then
    echo "=== Downloading gcc-$GCC_VER ==="
    wget -q "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"
fi

if [ ! -d "binutils-$BINUTILS_VER" ]; then
    echo "=== Extracting binutils ==="
    tar xf "binutils-$BINUTILS_VER.tar.xz"
fi
if [ ! -d "gcc-$GCC_VER" ]; then
    echo "=== Extracting gcc ==="
    tar xf "gcc-$GCC_VER.tar.xz"
fi

echo "=== Building binutils ==="
mkdir -p build-binutils
cd build-binutils
../binutils-$BINUTILS_VER/configure --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$(nproc)"
make install
cd "$SRC"

echo "=== Downloading GCC prerequisites ==="
cd "gcc-$GCC_VER"
./contrib/download_prerequisites
cd "$SRC"

echo "=== Building GCC (cross, freestanding) ==="
mkdir -p build-gcc
cd build-gcc
../gcc-$GCC_VER/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c,c++ --without-headers
make -j"$(nproc)" all-gcc
make -j"$(nproc)" all-target-libgcc
make install-gcc
make install-target-libgcc

# The C++ runtime (libsupc++ and the freestanding libstdc++) is built in a tree
# of its own, against the compiler just installed -- see the script for why.
# Without it C++ programs still compile but cannot throw, catch or ask typeid.
echo "=== Building the C++ runtime ==="
bash "$HERE/build_libstdcxx.sh"

echo "=== DONE ==="
"$PREFIX/bin/$TARGET-gcc" --version
