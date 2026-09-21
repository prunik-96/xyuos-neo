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
//   USER_STACK_BASE  stack, grows DOWN from USER_STACK_TOP
//
// Nothing here is mapped up front: pages are allocated on demand as segments
// are loaded and as the break moves, so a small program costs a few frames
// rather than the whole window.

#define USER_LOAD_BASE   (USER_VIRT_BASE + 0x00400000ULL)   /*  1 GiB +   4 MiB */
#define USER_HEAP_BASE   (USER_VIRT_BASE + 0x10000000ULL)   /*  1 GiB + 256 MiB */
#define USER_STACK_TOP   (USER_VIRT_BASE + 0x20000000ULL)   /*  1 GiB + 512 MiB */
#define USER_STACK_SIZE  0x00100000ULL                      /*  1 MiB           */
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_HEAP_LIMIT  USER_STACK_BASE

// True if [addr, addr + len) lies entirely inside the user window. Every
// syscall that dereferences a userland pointer must ask this first -- it is
// the only thing standing between a buggy (or hostile) program and the kernel.
int vmm_user_range_ok(uint64_t addr, uint64_t len);

#endif
