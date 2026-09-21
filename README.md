# xyuOS Neo

An x86_64 operating system written from nothing: own kernel, own libc, own
window manager, own TCP/IP stack, own TLS 1.3, own font rasteriser, own PNG
and JPEG decoders. No parts taken from another OS.

It boots on real hardware (UEFI, Ryzen 7 7700) and in QEMU, and it runs a
port of the NetSurf browser that fetches real sites over https and executes
JavaScript.

## Safety

**This system never touches the machine's internal NVMe disk.** It does not
format it, does not write to it, does not have a driver bound to it. Storage
is a RAM disk, a GRUB module, or an external drive. That is deliberate and
must stay true of anything added here.

## Building from nothing on a new machine

The repository holds about 12 MB: everything that was actually written here.
The 5.1 GB cross-compiler and the two upstream clones are not in it, because
both can be put back exactly.

Requires Debian or Ubuntu (WSL is fine) with `build-essential`, `bison`,
`flex`, `libgmp-dev`, `libmpc-dev`, `libmpfr-dev`, `texinfo`, `grub-pc-bin`,
`xorriso`, `qemu-system-x86`, `e2fsprogs`, `python3`.

```sh
git clone <this repo> ~/xyuos-neo     # the path matters -- see below
cd ~/xyuos-neo

./toolchain/build.sh                  # binutils 2.42 + GCC 13.2.0, ~40 min
python3 tools/vendor.py               # DOOM and MicroPython, at pinned commits
make                                  # kernel, libc, userland, ISO, disk image
```

That is enough to boot. To rebuild the browser as well:

```sh
python3 tools/nsfetch.py              # NetSurf 3.9 + its eight libraries
python3 tools/nscore.py               # the engine core
python3 tools/nsglue.py               # the xyuOS frontend and fetcher
python3 tools/nslink.py
python3 tools/nsinstall.py && make
```

### Where things are found

Nothing is hardcoded to one account. Each script in `tools/` locates the
project from its own position on disk, and the scratch area where the
NetSurf and Lexbor sources are unpacked defaults to `~/src`. Both can be
pointed elsewhere:

    XYUOS=/path/to/checkout  XYUOS_SRC=/path/to/sources  python3 tools/...

`toolchain/build.sh` is the one exception: it still installs into
`$HOME/xyuos-neo/toolchain/cross`, so clone to `~/xyuos-neo` or edit the two
`PREFIX` lines.

## Layout

| | |
|---|---|
| `kernel/` | the kernel: `arch/`, `mm/`, `fs/`, `net/`, `gfx/`, `wm/`, `crypto/`, `drivers/` |
| `libc/` | the C library, written here |
| `userland/` | programs, and the GUI toolkit they share |
| `disk/` | files that ship on the disk image |
| `assets/` | icons, sounds, wallpapers, the DOOM shareware WAD |
| `tools/` | build, test and measurement scripts — one per thing being measured |
| `third_party/nsxyuos/` | the NetSurf port: fetcher, frontend, plotters, chrome |
| `third_party/shim/` | zlib and iconv shims over our own code |
| `third_party/tcc/` | a C compiler that runs on the system itself |

## Where things stand

Working: SMP boot (8 cores online), paging with per-process address spaces,
ext2 and FAT32, xHCI and USB, a window manager with real windows, DHCP, DNS,
TCP, TLS 1.3 with certificate chain validation, HTTP with keep-alive, DOOM,
MicroPython, and NetSurf with JavaScript.

What is missing, and in what order to build it, is written down in the
roadmap: eight layers of platform work between here and a modern browser
engine — sockets, virtual memory, threads, processes, a C++ runtime, text
shaping, a path rasteriser, and a bigger disk.
