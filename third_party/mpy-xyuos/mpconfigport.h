/* MicroPython on xyuOS Neo.
 *
 * The system this runs on has a libc of its own making, a preemptive
 * scheduler and per-process address spaces, so this port looks much more like
 * the unix one than like a microcontroller: a real heap, a real filesystem,
 * argv, and an ordinary main(). What it does NOT have is threads, signals,
 * sockets from userland, or a dynamic loader, so everything resting on those
 * stays off.
 *
 * Exceptions unwind through setjmp/longjmp rather than the hand-written x86-64
 * NLR: ours is a freestanding target and the assembly version makes
 * assumptions about the stack that are not worth checking for the few
 * microseconds it saves.
 */

#include <stdint.h>

/* Everything the core offers short of the parts that need an OS we do not
 * have. Modules are switched off individually below rather than by dropping
 * to a lower level wholesale. */
#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

#define MICROPY_ALLOC_PATH_MAX          (256)
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_ENABLE_COMPILER         (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT  (1)
#define MICROPY_HELPER_REPL             (1)
#define MICROPY_REPL_AUTO_INDENT        (1)
#define MICROPY_REPL_EMACS_KEYS         (1)
#define MICROPY_KBD_EXCEPTION           (1)
#define MICROPY_ENABLE_SOURCE_LINE      (1)

#define MICROPY_NLR_SETJMP              (1)

/* Real numbers. The processor has SSE and the kernel saves FPU state per
 * process, so doubles cost nothing they would not cost anywhere else. */
#define MICROPY_FLOAT_IMPL              (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_MPZ)

/* Scripts are read through the filesystem layer above. */
#define MICROPY_PY_SYS_PLATFORM         "xyuos"
#define MICROPY_PY_SYS_PATH_DEFAULT     ".:/lib/python"

/* No threads, no processes started from Python, no sockets yet. */
#define MICROPY_PY_THREAD               (0)
#define MICROPY_PY_OS_DUPTERM           (0)
#define MICROPY_PY_SELECT               (0)
#define MICROPY_PY_SOCKET               (0)
#define MICROPY_PY_SSL                  (0)
#define MICROPY_PY_MACHINE              (0)
#define MICROPY_PY_BLUETOOTH            (0)
#define MICROPY_PY_NETWORK              (0)
/* The filesystem layer, on the POSIX calls libc now provides. This is what
 * gives os.listdir, os.stat, os.mkdir, os.remove and open() their real
 * behaviour instead of a port-specific imitation. */
#define MICROPY_VFS                     (1)
#define MICROPY_VFS_POSIX               (1)
#define MICROPY_VFS_WRITABLE            (1)
#define MICROPY_READER_VFS              (1)

/* The garbage collector needs to find roots on the C stack. */
#define MICROPY_GCREGS_SETJMP           (1)

/* The system errno.h carries the whole standard list, so the runtime
 * uses it rather than defining a parallel set. */
#define MICROPY_USE_INTERNAL_ERRNO      (0)
#define MICROPY_USE_INTERNAL_PRINTF     (0)

/* Types, sized for a 64-bit machine. */
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

#define MP_STATE_PORT MP_STATE_VM

/* The heap the collector manages, taken from the process heap at startup. */
#ifndef MICROPY_HEAP_SIZE
#define MICROPY_HEAP_SIZE               (16 * 1024 * 1024)
#endif
