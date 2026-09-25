#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include "paging.h"

// Virtual layout of a process, all inside the private user window
// [USER_VIRT_BASE, USER_VIRT_END) = [1 GiB, 2 GiB). Every process sees exactly
// these addresses; they mean different physical memory in each address space.
//
//   USER_LOAD_BASE   ELF image (userland/link.ld must match this)
//   USER_HEAP_BASE   heap, grows UP via SYS_SBRK
//   ...              the two grow towards each other and must not meet
//   USER_MMAP_TOP    mmap, placed DOWNWARD from here
//   ...              a guard page, mapped by nobody
//   USER_STACK_BASE  stack, grows DOWN from USER_STACK_TOP
//
// The stack used to sit in the middle of the window, at 512 MiB, with the
// whole upper half of the window unused and the heap boxed into 255 MiB
// beneath it. It is at the top now and the heap has the rest.
//
// Nothing here is mapped up front except the ELF image and the few pages of
// stack the kernel itself writes the arguments into. The heap below the
// break, the stack, and every mapping are declared and not built -- see
// vmm_fault().
//
// The window cannot simply be made larger. -mcmodel=small requires every
// symbol to sit below 2 GiB, and it is the only model tcc emits, so a program
// compiled ON this system has to run in the same window as one cross-compiled
// for it. Address space beyond 2 GiB is reachable only for memory addressed
// through pointers -- mappings, not the image -- and the free PDPT entries
// above 4 GiB are where that will go when it is needed.

#define USER_LOAD_BASE   (USER_VIRT_BASE + 0x00400000ULL)   /*  1 GiB +   4 MiB */
#define USER_HEAP_BASE   (USER_VIRT_BASE + 0x10000000ULL)   /*  1 GiB + 256 MiB */
#define USER_STACK_TOP   USER_VIRT_END                      /*  2 GiB           */
#define USER_STACK_SIZE  0x00800000ULL                      /*  8 MiB           */
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_SIZE)

// The kernel writes argc/argv into the top of the stack before the program
// runs, from ring 0, where a fault would not be a program's fault to take.
// So that much of it is mapped in the ordinary way and the rest is not.
#define USER_STACK_EAGER 0x00010000ULL                      /* 64 KiB */

// One page between the mappings and the stack, belonging to neither, so that
// a stack that runs too deep hits nothing and faults instead of quietly
// writing over something else.
#define USER_GUARD_SIZE  0x00001000ULL
#define USER_MMAP_TOP    (USER_STACK_BASE - USER_GUARD_SIZE)

// The heap may not grow past the lowest mapping, and a mapping may not be
// placed below the break. Which of them ends up with the room depends on
// which asks for it -- the two share the 760 MiB between them.
#define USER_HEAP_LIMIT  USER_MMAP_TOP

// --- what a program may do with a piece of memory --------------------------
// The same three bits userland spells PROT_READ / PROT_WRITE / PROT_EXEC.
#define VM_READ   1
#define VM_WRITE  2
#define VM_EXEC   4

// Mappings per address space. Every thread stack is one of these, so the
// number is really "mappings plus threads": thirty threads and a handful of
// mappings would not have fitted in thirty-two. The array lives in the
// address space, one per program rather than one per thread, and at
// twenty-four bytes each the whole table is under two kilobytes.
#define VM_REGIONS_MAX 64

typedef struct {
    uint64_t base;          // page-aligned; 0 when the slot is free
    uint64_t len;           // whole pages
    uint32_t prot;          // VM_READ / VM_WRITE / VM_EXEC
    uint32_t pad;
} vm_region_t;

// --- the memory of a process, as a thing in itself -------------------------
//
// Held by pointer rather than by value because it can have more than one
// holder: threads of one program share exactly this, and nothing else. The
// count is what lets any of them exit first, in any order, without the
// address space going out from under the others.
typedef struct addr_space {
    uint64_t    pml4;       // physical address of the page tables
    uint64_t    brk;        // heap break
    vm_region_t vm[VM_REGIONS_MAX];
    int         refs;       // how many processes share this
} addr_space_t;

// A fresh address space with page tables of its own, held once. Returns NULL
// if either the object or the tables could not be had.
addr_space_t *vmm_space_new(void);

// One more holder, and one fewer. The last unref frees the page tables, every
// user page in them, and the object.
void vmm_space_ref(addr_space_t *as);
void vmm_space_unref(addr_space_t *as);

// True if [addr, addr + len) lies entirely inside the user window. Every
// syscall that dereferences a userland pointer must ask this first -- it is
// the only thing standing between a buggy (or hostile) program and the kernel.
int vmm_user_range_ok(uint64_t addr, uint64_t len);

// A page fault from the running process. `err` is the processor's error code,
// which says what kind of fault this was:
//
//   bit 0  the page was PRESENT -- so this is a refusal, not a gap
//   bit 1  the access was a write
//   bit 4  the access was an instruction fetch
//
// Returns 1 if the address belonged to a region that is built on demand and a
// page has now been put there, so the instruction can simply be tried again;
// 0 if the program should be stopped. Answering 1 for a refusal would put the
// processor in a loop taking the same fault forever.
#define PF_PRESENT  0x1
#define PF_WRITE    0x2
#define PF_FETCH    0x10

int vmm_fault(uint64_t addr, uint64_t err);

// --- mappings --------------------------------------------------------------
// Anonymous only: there is no file-backed mapping here, and a program that
// wants a file's contents reads them. `prot` is the VM_* bits.
//
// A new mapping is placed as high as it will go, below whatever is already
// mapped, and no pages are built until they are touched.
uint64_t vmm_mmap(uint64_t len, uint32_t prot);
int      vmm_munmap(uint64_t addr, uint64_t len);
int      vmm_mprotect(uint64_t addr, uint64_t len, uint32_t prot);

// The lowest address any mapping occupies, or USER_MMAP_TOP when there are
// none. This is where the heap has to stop.
uint64_t vmm_mmap_floor(void);

#endif
