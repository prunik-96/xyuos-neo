CROSS   := $(HOME)/xyuos-neo/toolchain/cross/bin/x86_64-elf-
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
LD      := $(CROSS)gcc

DEPFLAGS := -MMD -MP
CFLAGS  := $(DEPFLAGS) -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mgeneral-regs-only -Wall -Wextra -std=gnu11 -O2 -g
# kernel/gfx (font renderer: stb_truetype + kmath + font.c) needs floating
# point, so it drops -mgeneral-regs-only and enables SSE. Warnings silenced
# because stb_truetype is vendored third-party code. SSE is turned on at boot.
GFX_CFLAGS := $(DEPFLAGS) -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
              -mno-red-zone -msse -msse2 -std=gnu11 -O2 -g -w
LDFLAGS := -T kernel/arch/x86_64/linker.ld -ffreestanding -nostdlib -no-pie -O2

# Raw-syscall userland programs (no libc, use userland/syscalls.h directly).
USER_CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                -mno-red-zone -mgeneral-regs-only -Wall -Wextra -std=gnu11 -O2 -g
USER_LDFLAGS := -T userland/link.ld -ffreestanding -nostdlib -no-pie -static -O2

# libc itself, and libc-based userland programs.
LIBC_CFLAGS   := $(USER_CFLAGS) -Ilibc/include
LIBC_CXXFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                 -mno-red-zone -mgeneral-regs-only -Wall -Wextra -std=gnu++17 \
                 -fno-exceptions -fno-rtti -O2 -g -Ilibc/include
LIBC_LDFLAGS  := -T userland/link.ld -ffreestanding -nostdlib -no-pie -static -O2

SRC_C := $(shell find kernel -name '*.c')
KDEPS := $(SRC_C:.c=.d)
SRC_S := $(shell find kernel -name '*.S')
OBJ   := $(SRC_C:.c=.o) $(SRC_S:.S=.o)

KERNEL := build/xyuos_neo.elf
ISO    := build/xyuos_neo.iso

RAW_USER_PROGS  := test1
LIBC_C_PROGS    := files note view taskmgr play hello_c fstest spin parent keywait sh fm edit cc run \
                   ls cat echo wc grep head tail sort uniq tee hexdump \
                   cp mv touch stat ps free uname sleep kill loop bigfile yes count crash \
                   plasma devmgr control web ftest netlog
LIBC_CXX_PROGS  := hello_cpp
# The interactive shell / file manager / editor now live in the kernel WM pane
# engine (kernel/wm/), so there are no separate userland shell binaries; these
# remaining userland programs are just the boot demos.
ALL_USER_PROGS  := $(RAW_USER_PROGS) $(LIBC_C_PROGS) $(LIBC_CXX_PROGS)
USER_ELFS       := $(patsubst %,build/%.elf,$(ALL_USER_PROGS))

LIBC_SRCS := libc/src/syscalls.c libc/src/stdio.c libc/src/stdlib.c libc/src/string.c \
             libc/src/string_extra.c libc/src/posix.c libc/src/scanf.c \
             libc/src/math.c libc/src/readline.c libc/src/dirstat.c \
             libc/src/inet.c libc/src/regex.c \
             libc/src/timecal.c libc/src/posixbits.c libc/src/mman.c
LIBC_OBJS := $(patsubst libc/src/%.c,build/libc_%.o,$(LIBC_SRCS))
LIBC_CXX_OBJS := build/libc_cxxabi_stubs.o
LIBC_A    := build/libc.a
CRT0      := build/crt0.o

.PHONY: all run clean

all: $(ISO)

# Font renderer TUs (SSE, no -mgeneral-regs-only). More specific than %.o so
# make prefers this rule for kernel/gfx.
kernel/gfx/%.o: kernel/gfx/%.c
	$(CC) $(GFX_CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) kernel/arch/x86_64/linker.ld
	mkdir -p build
	$(LD) $(LDFLAGS) $(OBJ) -o $@ -lgcc

# --- raw-syscall userland programs (test1, ls, cat) ---

build/%_user.o: userland/%.c userland/syscalls.h
	mkdir -p build
	$(CC) $(USER_CFLAGS) -c $< -o $@

# --- libc ---

build/libc_%.o: libc/src/%.c
	mkdir -p build
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

# posix.c does real floating-point work (ldexp, strtod), which cannot be built
# under -mgeneral-regs-only. Safe in ring3: the kernel enables SSE before any
# user code runs. An explicit target rule beats the pattern rule above.
LIBC_FP_CFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                  -mno-red-zone -msse -msse2 -Wall -Wextra -std=gnu11 -O2 -g \
                  -Ilibc/include

build/libc_posix.o: libc/src/posix.c
	mkdir -p build
	$(CC) $(LIBC_FP_CFLAGS) -c $< -o $@

# scanf.c parses floating point via strtod, and stdio.c formats it for %f/%e/%g;
# both need SSE and cannot be built under -mgeneral-regs-only.
build/libc_scanf.o: libc/src/scanf.c
	mkdir -p build
	$(CC) $(LIBC_FP_CFLAGS) -c $< -o $@

# The elementary functions are floating point from top to bottom.
build/libc_math.o: libc/src/math.c $(wildcard libc/include/*.h)
	mkdir -p build
	$(CC) $(LIBC_FP_CFLAGS) -c $< -o $@

build/libc_stdio.o: libc/src/stdio.c
	mkdir -p build
	$(CC) $(LIBC_FP_CFLAGS) -c $< -o $@

build/libc_cxxabi_stubs.o: libc/src/cxxabi_stubs.cpp
	mkdir -p build
	$(CXX) $(LIBC_CXXFLAGS) -c $< -o $@

$(CRT0): libc/src/crt0.S
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# setjmp/longjmp must be assembly: they save and restore callee-saved
# registers, the stack pointer and the return address by hand.
LIBC_ASM_OBJS := build/libc_setjmp.o

build/libc_setjmp.o: libc/src/setjmp.S
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBC_A): $(LIBC_OBJS) $(LIBC_CXX_OBJS) $(LIBC_ASM_OBJS)
	$(CROSS)ar rcs $@ $(LIBC_OBJS) $(LIBC_CXX_OBJS) $(LIBC_ASM_OBJS)

# --- libc-based userland programs ---

build/%_libcuser.o: userland/%.c $(wildcard libc/include/*.h) $(wildcard userland/*.h)
	mkdir -p build
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

# Userland programs that do floating-point arithmetic themselves need SSE:
# under -mgeneral-regs-only GCC emits soft-float calls (__gtdf2 and friends)
# that libgcc for this target does not provide. Add a program here if it starts
# using doubles. Programs that merely PASS doubles to printf are fine, since
# the conversion happens inside libc.
FP_USER_PROGS := fstest sleep ftest web

define FP_OBJ_RULE
build/$(1)_libcuser.o: userland/$(1).c $$(wildcard libc/include/*.h) $$(wildcard userland/*.h)
	mkdir -p build
	$$(CC) $$(LIBC_FP_CFLAGS) -c $$< -o $$@
endef
$(foreach p,$(FP_USER_PROGS),$(eval $(call FP_OBJ_RULE,$(p))))

build/%_libcuser.o: userland/%.cpp $(wildcard libc/include/*.h)
	mkdir -p build
	$(CXX) $(LIBC_CXXFLAGS) -c $< -o $@

define LIBC_LINK_RULE
build/$(1).elf: build/$(1)_libcuser.o $(CRT0) $(LIBC_A) userland/link.ld
	$(LD) $(LIBC_LDFLAGS) $(CRT0) build/$(1)_libcuser.o $(LIBC_A) -o $$@ -lgcc
endef
$(foreach p,$(LIBC_C_PROGS) $(LIBC_CXX_PROGS),$(eval $(call LIBC_LINK_RULE,$(p))))

build/%.elf: build/%_user.o userland/link.ld
	$(LD) $(USER_LDFLAGS) $< -o $@

# --- the C compiler (vendored TinyCC, built as a userland program) ---
#
# Built from third_party/tcc with ONE_SOURCE=1: tcc.c #includes the rest of the
# compiler, so this single TU is the whole thing. It links against our own libc
# like any other program -- that is the entire "port".
TCC_SRC := third_party/tcc
TCC_CFLAGS := -DTCC_TARGET_X86_64 -DONE_SOURCE=1 -DCONFIG_TCC_STATIC \
              -DCONFIG_TCC_PREDEFS=1 \
              -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
              -mno-red-zone -msse -msse2 -std=gnu11 -O1 -w \
              -I$(TCC_SRC) -Ilibc/include

build/tcc_main.o: $(TCC_SRC)/tcc.c $(TCC_SRC)/config.h
	mkdir -p build
	$(CC) $(TCC_CFLAGS) -c $< -o $@

build/tcc.elf: build/tcc_main.o $(CRT0) $(LIBC_A) userland/link.ld
	$(LD) $(LIBC_LDFLAGS) $(CRT0) build/tcc_main.o $(LIBC_A) -o $@ -lgcc

# --- DOOM (vendored doomgeneric), built as a userland program ---
# The object list is doomgeneric's own generic set, with its X11 backend
# swapped for our platform layer (doomgeneric_xyuos.c). Built with SSE (DOOM
# does a little floating point) and warnings off (third-party code).
DOOM_SRC   := third_party/doomgeneric/doomgeneric
DOOM_NAMES := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad \
  d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info \
  i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv \
  m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng \
  p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats \
  p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user \
  r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds \
  statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum \
  w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric
DOOM_OBJS  := $(patsubst %,build/doom/%.o,$(DOOM_NAMES)) build/doom/doomgeneric_xyuos.o
DOOM_CFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
               -mno-red-zone -msse -msse2 -std=gnu11 -O2 -w \
               -DNORMALUNIX -I$(DOOM_SRC) -Ilibc/include

build/doom/%.o: $(DOOM_SRC)/%.c
	mkdir -p build/doom
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

build/doom/doomgeneric_xyuos.o: userland/doomgeneric_xyuos.c
	mkdir -p build/doom
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

build/doom.elf: $(DOOM_OBJS) $(CRT0) $(LIBC_A) userland/link.ld
	$(LD) $(LIBC_LDFLAGS) $(CRT0) $(DOOM_OBJS) $(LIBC_A) -o $@ -lgcc

MPY_PORT := third_party/mpy-xyuos

# MicroPython builds itself: its make generates the interned-string tables and
# the module registry from the sources before compiling any of them. It links
# against our libc and crt0, so those come first.
build/python.elf: $(LIBC_A) $(CRT0) $(wildcard $(MPY_PORT)/*.c) $(wildcard $(MPY_PORT)/*.h)
	$(MAKE) -C $(MPY_PORT) XYUOS=$(CURDIR)
	cp $(MPY_PORT)/build/python.elf $@

.PHONY: python-clean
python-clean:
	$(MAKE) -C $(MPY_PORT) clean

$(ISO): $(KERNEL) $(USER_ELFS) build/python.elf grub.cfg disk.img
	mkdir -p build/isodir/boot/grub
	cp $(KERNEL) build/isodir/boot/xyuos_neo.elf
	cp grub.cfg build/isodir/boot/grub/grub.cfg
	# The filesystem image ships in the ISO as a GRUB module, so the OS has a
	# disk on bare metal (a RAM disk) with no storage driver at all.
	cp disk.img build/isodir/boot/disk.img
	grub-mkrescue -o $@ build/isodir 2>/dev/null

# Userland lives on the filesystem now (P2), so the disk image depends on the
# program binaries. NOTE: disk.img has no dependency on the Makefile itself --
# after changing the recipe, `rm -f disk.img` before rebuilding.
# The icon set, decoded and scaled on the build machine: the kernel draws
# the taskbar and has no PNG decoder, and should not grow one just to put
# a picture on a button. See tools/mkicons.py.
assets/icons.bin: $(wildcard assets/icons/*.png) tools/mkicons.py
	python3 tools/mkicons.py assets/icons assets/icons.bin

disk.img: disk/hello.txt disk/about.txt disk/t1.c disk/demo.c disk/calc.c \
          disk/bad1.c disk/bad2.c disk/readme.txt assets/icons.bin \
          $(wildcard assets/pics/*) $(wildcard assets/sounds/*) \
          $(USER_ELFS) build/tcc.elf build/python.elf $(wildcard disk/python/*.py) \
          build/doom.elf assets/doom1.wad \
          $(CRT0) $(LIBC_A) $(wildcard libc/include/*.h) \
          $(wildcard third_party/nsxyuos/netsurf.elf) \
          $(wildcard third_party/nsxyuos/res/*)
	rm -f disk.img
	# 32 MiB: holds /bin (tcc ~378K, doom ~600K), the DOOM WAD (~4 MiB), /lib,
	# the headers, and user files. On bare metal the whole image is loaded into
	# RAM as an initrd shipped inside the ISO, so every megabyte is boot time.
	dd if=/dev/zero of=disk.img bs=1M count=32 status=none
	mke2fs -q -F -b 1024 -O ^resize_inode disk.img
	debugfs -w -R "write disk/hello.txt hello.txt" disk.img
	debugfs -w -R "write disk/about.txt about.txt" disk.img
	debugfs -w -R "write disk/t1.c t1.c" disk.img
	debugfs -w -R "write disk/demo.c demo.c" disk.img
	debugfs -w -R "write disk/calc.c calc.c" disk.img
	debugfs -w -R "write disk/bad1.c bad1.c" disk.img
	debugfs -w -R "write disk/bad2.c bad2.c" disk.img
	for s in disk/s?.c; do \
	    debugfs -w -R "write $$s `basename $$s`" disk.img; \
	done
	debugfs -w -R "mkdir /bin" disk.img
	# The shell's file-backed pipes stage their intermediate output here.
	debugfs -w -R "mkdir /tmp" disk.img
	# Somewhere for the user's own files, and something for the viewer to open.
	debugfs -w -R "mkdir /home" disk.img
	debugfs -w -R "mkdir /pics" disk.img
	debugfs -w -R "mkdir /sounds" disk.img
	for f in assets/sounds/*; do \
	    debugfs -w -R "write $$f /sounds/`basename $$f`" disk.img; \
	done
	for f in assets/pics/*; do \
	    debugfs -w -R "write $$f /pics/`basename $$f`" disk.img; \
	done
	debugfs -w -R "write disk/readme.txt /home/readme.txt" disk.img
	for p in $(ALL_USER_PROGS); do \
	    debugfs -w -R "write build/$$p.elf /bin/$$p" disk.img; \
	done
	debugfs -w -R "write build/tcc.elf /bin/tcc" disk.img
	debugfs -w -R "write build/doom.elf /bin/doom" disk.img
	debugfs -w -R "write assets/doom1.wad /doom1.wad" disk.img
	# Icons are built but not installed for now. Uncomment to switch them on.
	# debugfs -w -R "write assets/icons.bin /icons.bin" disk.img
	# What the compiler needs to build a program ON the OS: the startup file
	# and libc to link against, and the headers to include.
	debugfs -w -R "mkdir /lib" disk.img
	debugfs -w -R "write $(CRT0) /lib/crt0.o" disk.img
	debugfs -w -R "write $(LIBC_A) /lib/libc.a" disk.img
	debugfs -w -R "mkdir /include" disk.img
	for h in libc/include/*.h; do \
	    debugfs -w -R "write $$h /include/`basename $$h`" disk.img; \
	done
	debugfs -w -R "mkdir /include/sys" disk.img
	for h in libc/include/sys/*.h; do \
	    debugfs -w -R "write $$h /include/sys/`basename $$h`" disk.img; \
	done
	# tcc ships its own stdarg.h/stddef.h/float.h and expects them under
	# CONFIG_TCCDIR (see third_party/tcc/config.h). They are the compiler's,
	# not the libc's, and must not be confused with /include.
	debugfs -w -R "mkdir /tcc" disk.img
	debugfs -w -R "mkdir /tcc/include" disk.img
	for h in $(TCC_SRC)/include/*.h; do \
	    debugfs -w -R "write $$h /tcc/include/`basename $$h`" disk.img; \
	done

	# NetSurf, if it has been built. See tools/nsbuild.py and the scripts
	# beside it; its sources are not in this tree, so a clone without them
	# still builds a whole system and simply has one fewer browser.
	#
	# The stylesheets matter as much as the binary: default.css is the user
	# agent stylesheet, and without it NetSurf has no block layout at all.
	@if [ -f third_party/nsxyuos/netsurf.elf ]; then \
	    debugfs -w -R "write third_party/nsxyuos/netsurf.elf /bin/netsurf" disk.img >/dev/null 2>&1; \
	    debugfs -w -R "mkdir /lib/netsurf" disk.img >/dev/null 2>&1; \
	    for f in third_party/nsxyuos/res/*; do \
	        test -f $$f && debugfs -w -R "write $$f /lib/netsurf/`basename $$f`" disk.img >/dev/null 2>&1; \
	    done; \
	    debugfs -w -R "mkdir /lib/netsurf/icons" disk.img >/dev/null 2>&1; \
	    for f in third_party/nsxyuos/res/icons/*; do \
	        test -f $$f && debugfs -w -R "write $$f /lib/netsurf/icons/`basename $$f`" disk.img >/dev/null 2>&1; \
	    done; \
	    echo "netsurf installed into the disk image"; \
	else \
	    echo "netsurf not built; skipping (see tools/nsbuild.py)"; \
	fi

	# Python, and somewhere for its libraries to live. The binary is stripped
	# on the way in: its debug info is four times the size of the program
	# itself, and the disk is thirty-two megabytes.
	$(CROSS)strip -o build/python.stripped build/python.elf
	debugfs -w -R "write build/python.stripped /bin/python" disk.img
	debugfs -w -R "mkdir /lib/python" disk.img
	for f in $(wildcard disk/python/*.py); do \
	    debugfs -w -R "write $$f /lib/python/`basename $$f`" disk.img; \
	done

QEMU_DISK := -drive file=disk.img,if=none,id=disk0,format=raw -device virtio-blk-pci,drive=disk0

# Hardware virtualisation, when this machine will allow it.
#
# Without it QEMU interprets every single instruction, and the whole desktop
# runs one or two orders of magnitude slower than the same code does on real
# hardware -- which reads as "the graphics lag", because they do. If this
# comes out empty, /dev/kvm exists but is not readable: add yourself to the
# kvm group once, with
#     sudo usermod -aG kvm $$USER
# and start a new shell.
QEMU_ACCEL := $(shell [ -r /dev/kvm ] && [ -w /dev/kvm ] && echo -enable-kvm -cpu host)

# A network card. Without one the whole IP stack is dead code: nic_init()
# probes, finds nothing, and net_up() fails before it can send a single frame.
# The e1000 is QEMU's 82540EM, which is the card our e1000 driver was written
# against; the real machine's Realtek is a different driver on the same nic.h
# seam, so what works here is the stack, not the chip.
#
# -netdev user is QEMU's own userspace NAT: the guest sees a 10.0.2.x network
# with the gateway at 10.0.2.2, a DHCP server, and a DNS forwarder at 10.0.2.3.
# It needs no root and no host configuration.
#
#   make run NETDUMP=1     also writes every frame to net.pcap (open in
#                          Wireshark) -- the way to see what the stack really
#                          put on the wire when it does not work.
QEMU_NET := -netdev user,id=n0 -device e1000,netdev=n0
ifdef NETDUMP
QEMU_NET += -object filter-dump,id=dump0,netdev=n0,file=net.pcap
endif

# -device usb-mouse gives the USB HID path something to find; the emulated
# PS/2 pair is there regardless, so both pointer drivers get exercised.
QEMU_USB := -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
            -device usb-mouse,bus=xhci.0

run: $(ISO) disk.img
	@[ -n "$(QEMU_ACCEL)" ] || echo "note: running without KVM (slow). sudo usermod -aG kvm $$USER, then a new shell."
	qemu-system-x86_64 $(QEMU_ACCEL) -cdrom $(ISO) -serial stdio -m 512M $(QEMU_DISK) $(QEMU_USB) $(QEMU_NET)

run-nographic: $(ISO) disk.img
	qemu-system-x86_64 $(QEMU_ACCEL) -cdrom $(ISO) -serial stdio -m 512M -display none $(QEMU_DISK) $(QEMU_USB) $(QEMU_NET)

# Simulate bare metal: NO virtio drive, so the OS must use the RAM-disk GRUB
# module instead. This is the path real hardware takes.
run-ram: $(ISO)
	qemu-system-x86_64 $(QEMU_ACCEL) -cdrom $(ISO) -serial stdio -m 512M $(QEMU_USB) $(QEMU_NET)

-include $(KDEPS)

clean:
	rm -f $(OBJ) $(KDEPS)
	rm -rf build
	rm -f disk.img
