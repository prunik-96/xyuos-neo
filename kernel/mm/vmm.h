#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include "paging.h"

// Virtual layout of a process, all inside the private user window
// [USER_VIRT_BASE, USER_VIRT_END) = [1 GiB, 2 GiB). Every process sees exactly
// these addresses; they mean different physical memory in each address space.
//
//   USER_LOAD_BASE   ELF image (userland/link.ld must match this)
//   USER_HEAP_BASE   heap, grows UP via SYS_SBRK, stops at USER_HEAP_LIMIT
//   ...              a guard page, mapped by nobody
//   USER_STACK_BASE  stack, grows DOWN from USER_STACK_TOP
//
// The stack used to sit in the middle of the window, at 512 MiB, with the
// whole upper half of the window unused and the heap boxed into 255 MiB
// beneath it. It is at the top now and the heap has the rest: 760 MiB.
//
// Nothing here is mapped up front except the ELF image and the few pages of
// stack the kernel itself writes the arguments into. The heap below the break
// and the rest of the stack are declared and not built -- see vmm_fault().
//
// The window cannot simply be made larger. -mcmodel=small requires every
// symbol to sit below 2 GiB, and it is the only model tcc emits, so a program
// compiled ON this system has to run in the same window as one cross-compiled
// for it. Address space beyond 2 GiB is reachable only for memory addressed
// through pointers -- mmap, not the image -- and that is a later step.

#define USER_LOAD_BASE   (USER_VIRT_BASE + 0x00400000ULL)   /*  1 GiB +   4 MiB */
#define USER_HEAP_BASE   (USER_VIRT_BASE + 0x10000000ULL)   /*  1 GiB + 256 MiB */
#define USER_STACK_TOP   USER_VIRT_END                      /*  2 GiB           */
#define USER_STACK_SIZE  0x00800000ULL                      /*  8 MiB           */
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_SIZE)

// The kernel writes argc/argv into the top of the stack before the program
// runs, from ring 0, where a fault would not be a program's fault to take.
// So that much of it is mapped in the ordinary way and the rest is not.
#define USER_STACK_EAGER 0x00010000ULL                      /* 64 KiB */

// One page between the two, belonging to neither, so that a stack that runs
// too deep or a heap that grows too far hits nothing and faults, instead of
// quietly writing over the other.
#define USER_GUARD_SIZE  0x00001000ULL
#define USER_HEAP_LIMIT  (USER_STACK_BASE - USER_GUARD_SIZE)

// True if [addr, addr + len) lies entirely inside the user window. Every
// syscall that dereferences a userland pointer must ask this first -- it is
// the only thing standing between a buggy (or hostile) program and the kernel.
int vmm_user_range_ok(uint64_t addr, uint64_t len);

// A page fault from the running process. Returns 1 if the address belonged to
// a region that is built on demand and a page has now been put there, so the
// instruction can simply be tried again; 0 if the address belongs to nothing
// and the program should be stopped.
int vmm_fault(uint64_t addr, int write);

#endif
