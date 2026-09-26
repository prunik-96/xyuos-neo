# xyuOS Neo

An x86_64 operating system written from nothing: own kernel, own libc, own
window manager, own TCP/IP stack, own TLS 1.3, own PNG and JPEG decoders.
No parts taken from another OS.

It boots on real hardware (UEFI, Ryzen 7 7700) and in QEMU, and it runs a
port of the NetSurf browser that fetches real sites over https, executes
JavaScript, and sets text in every major script of the world.

Libraries by other people that programs link against are named where they
are used: FreeType, HarfBuzz, SheenBidi and libunibreak under the text, the
Noto fonts on the disk, and the programs ported to the system (NetSurf,
DOOM, MicroPython, TinyCC).

## Safety

**This system never touches the machine's internal NVMe disk.** It does not
format it, does not write to it, does not have a driver bound to it. Storage
is the USB stick it boots from, a RAM disk, or an external drive. That is
deliberate and must stay true of anything added here.

On the stick, the system writes to one partition only: the ext2 partition
the build appends to the ISO, recognised by a UUID made fresh for each build
(`build/diskid`). A stick written by another build, a data stick, a backup
drive -- anything without that exact filesystem -- is never written to.

## Writing a stick

`build/xyuos_neo.iso` is a hybrid image: write it to the stick as it is --
Rufus in **DD image** mode, or `dd` -- not file by file. Everything on the
stick is replaced. The system then boots from it (UEFI, no CSM needed) and
its disk is the 1 GiB partition at the end of the image: what you write there
is still there after a reboot. Windows will say it cannot read the stick; that
is expected, do not let it format it.

If the stick's disk cannot be used on some machine, the boot menu's second
entry, "disk in memory", loads the same files into RAM, where they last until
the machine is switched off.

## Building from nothing on a new machine

The repository holds about 12 MB: everything that was actually written here.
The 5.1 GB cross-compiler, the upstream clones and the fonts are not in it,
because all of them can be put back exactly.

Requires Debian or Ubuntu (WSL is fine) with `build-essential`, `bison`,
`flex`, `libgmp-dev`, `libmpc-dev`, `libmpfr-dev`, `texinfo`, `grub-pc-bin`,
`xorriso`, `qemu-system-x86`, `e2fsprogs`, `python3`.

```sh
git clone <this repo> ~/xyuos-neo     # the path matters -- see below
cd ~/xyuos-neo

./toolchain/build.sh                  # binutils 2.42 + GCC 13.2.0 + C++ runtime, ~50 min
python3 tools/vendor.py               # DOOM, MicroPython, the text libraries at
                                      # pinned commits; 48 Noto fonts by checksum
make                                  # kernel, libc, libtext, userland, ISO, disk image
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
| `libc/` | the C library, written here; `libc/include/c++/` wraps it for C++ |
| `libtext/` | text for programs: fonts chosen per character, shaping, bidi, line breaking — over FreeType, HarfBuzz, SheenBidi and libunibreak |
| `userland/` | programs, and the GUI toolkit they share |
| `disk/` | files that ship on the disk image |
| `assets/` | icons, sounds, wallpapers, the DOOM shareware WAD |
| `tools/` | build, test and measurement scripts — one per thing being measured |
| `third_party/nsxyuos/` | the NetSurf port: fetcher, frontend, plotters, chrome |
| `third_party/shim/` | zlib and iconv shims over our own code |
| `third_party/tcc/` | a C compiler that runs on the system itself |

## Where things stand

Working: programs running on every core at once (one big kernel lock, so
user code is parallel and the kernel is not), threads, signals, System V
shared memory, per-process address spaces with demand paging, mmap and W^X,
C++ with exceptions and RTTI, ext2 and FAT32, xHCI and USB, a window manager
with real windows, DHCP, DNS, TCP with several connections at once, TLS 1.3
with certificate chain validation, HTTP with keep-alive, text shaping for
every major script (kerning, ligatures, Arabic joining, Indic reordering,
bidirectional text, Unicode line breaking), DOOM, MicroPython, and NetSurf
with JavaScript.

The platform work between here and a modern browser engine was planned as
eight layers. Seven are done: parallel sockets, virtual memory, threads and
scheduling on every core, processes and IPC, the C++ runtime, text, and a
bigger disk -- a 1 GiB partition on the boot stick, persistent, with a
128 MiB image in memory as the fallback (see `STICK_MB` and `DISK_MB` in the
Makefile). One is left: a path rasteriser.

The tests are programs on the disk (`fstest`, `sigtest`, `shmtest`,
`thrtest`, `partest`, `cpptest`, `texttest`, `memtest`, `vmtest`), driven
from the host by `tools/seqrun.py`, which types commands into the shell and
photographs the screen. Set `SEQRUN_SMP=8` to run them on eight cores: QEMU
gives one unless told otherwise. `SEQRUN_STICK=1` boots the way real hardware
does, from the ISO as a USB stick, and `SEQRUN_UEFI=1` with UEFI firmware;
`SEQRUN_KEEP=1` keeps the stick from the last run, to see what survived.
`SEQRUN_RAMDISK=1` runs from the in-memory disk. `fonts` opens a window of
text in every script, for the eye.
