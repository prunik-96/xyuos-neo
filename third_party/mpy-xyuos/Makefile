# MicroPython for xyuOS Neo.
#
# Cross-compiled with the same toolchain, linker script, crt0 and libc as
# every other program on the system: this is an ordinary userland binary, not
# firmware. XYUOS points at the top of the OS tree.

# MicroPython derives TOP from the path this include is written with, so
# pointing MPY_TOP at their tree is all it takes for the port to build from
# outside it. The default is the sibling directory tools/vendor.py clones.
MPY_TOP ?= ../micropython

include $(MPY_TOP)/py/mkenv.mk

XYUOS ?= $(abspath ../..)

QSTR_DEFS = qstrdefsport.h

MICROPY_ROM_TEXT_COMPRESSION ?= 1

include $(TOP)/py/py.mk
include $(TOP)/extmod/extmod.mk

CROSS_COMPILE ?= $(XYUOS)/toolchain/cross/bin/x86_64-elf-

INC += -I.
INC += -I$(TOP)
INC += -I$(BUILD)
INC += -I$(XYUOS)/libc/include

# Freestanding, no red zone, and SSE on: the language has real numbers in it,
# and the kernel saves FPU state per process so using them costs nothing.
CFLAGS += $(INC) -Wall -std=gnu11 -ffreestanding -fno-stack-protector \
          -fno-pic -fno-pie -mno-red-zone -msse -msse2 $(COPT)
CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-type-limits

# MicroPython has a readline() of its own, with a different signature.
# Ours is a libc extension, so it is the one that gets out of the way.
CFLAGS += -DXYUOS_NO_READLINE

# alloca is the compiler's, not the library's: the memory has to come from
# the caller's own frame, which only the compiler can arrange.
CFLAGS += -Dalloca=__builtin_alloca

LDFLAGS += -T $(XYUOS)/userland/link.ld -ffreestanding -nostdlib -no-pie -static

ifeq ($(DEBUG), 1)
CFLAGS += -O0 -g
else
CFLAGS += -Os -DNDEBUG -g
CFLAGS += -fdata-sections -ffunction-sections
LDFLAGS += -Wl,--gc-sections
endif

LD = $(CC)
LIBS = $(XYUOS)/build/libc.a -lgcc

SRC_C = \
	main.c \
	mphalport.c \
	shared/readline/readline.c \
	shared/runtime/gchelper_generic.c \
	shared/runtime/pyexec.c \
	shared/runtime/stdout_helpers.c \

SRC_QSTR += shared/readline/readline.c shared/runtime/pyexec.c

OBJ += $(PY_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

# crt0 supplies _start and calls main; it is not compiled here.
CRT0 = $(XYUOS)/build/crt0.o

all: $(BUILD)/python.elf

$(BUILD)/python.elf: $(OBJ) $(CRT0)
	$(ECHO) "LINK $@"
	$(Q)$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJ) $(LIBS)
	$(Q)$(SIZE) $@

include $(TOP)/py/mkrules.mk
