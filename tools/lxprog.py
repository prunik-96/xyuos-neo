#!/usr/bin/env python3
"""Build the Lexbor test program for xyuOS and put it on the disc.

Separate from the main Makefile for the same reason NetSurf is: the library's
sources live outside this tree. Run tools/lxbuild.py first.
"""
import os, subprocess, sys

HOME = "/home/roman/xyuos-neo"
LX = "/home/roman/src/lexbor-2.4.0"
LIB = "/home/roman/src/liblexbor.a"
CC = HOME + "/toolchain/cross/bin/x86_64-elf-gcc"
LD = HOME + "/toolchain/cross/bin/x86_64-elf-ld"
OUT = "/home/roman/src/lxprog"

if not os.path.exists(LIB):
    sys.exit("no liblexbor.a -- run tools/lxbuild.py first")

os.makedirs(OUT, exist_ok=True)

FLAGS = ["-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
         "-mno-red-zone", "-msse", "-msse2", "-std=gnu11", "-O2",
         "-Wall", "-Wextra", "-Wno-unused-parameter", "-DLEXBOR_STATIC"]

INC = ["-I" + HOME + "/libc/include", "-I" + LX + "/source"]

obj = OUT + "/lxtest.o"
r = subprocess.run([CC] + FLAGS + INC + ["-c", HOME + "/userland/lxtest.c",
                                         "-o", obj],
                   capture_output=True, text=True)
if r.returncode != 0:
    sys.exit("the test program did not compile:\n" + r.stderr)
if r.stderr.strip():
    print("warnings:\n" + r.stderr.strip())

elf = OUT + "/lxtest.elf"
r = subprocess.run([LD, "-nostdlib", "-static", "-o", elf,
                    "-T", HOME + "/userland/link.ld",
                    HOME + "/build/crt0.o", obj, LIB,
                    HOME + "/build/libc.a", LIB, HOME + "/build/libc.a"],
                   capture_output=True, text=True)
if r.returncode != 0:
    print("the link did not complete:")
    for line in r.stderr.splitlines():
        if "undefined reference" in line or "error" in line.lower():
            print("   " + line.strip())
    sys.exit(1)

subprocess.run([HOME + "/toolchain/cross/bin/x86_64-elf-strip",
                "-o", OUT + "/lxtest", elf], check=True)
print("linked: %.2f MB (%.2f stripped)"
      % (os.path.getsize(elf) / 1e6, os.path.getsize(OUT + "/lxtest") / 1e6))
print(OUT + "/lxtest")
