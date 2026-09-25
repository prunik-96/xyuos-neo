#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

// Per-process address spaces.
//
// Layout decision: the kernel is NOT moved to the higher half, and user space
// is NOT put at some far-away virtual base either. Both would be tidier, but
// both break the small code model: -mcmodel=small (the default, and the only
// one tcc emits) needs every symbol to live below 2 GiB. Programs compiled ON
// xyuOS have to run in the same window as programs cross-compiled for it, so
// the user window has to stay inside the low 2 GiB.
//
// So the split is made one level down, at the PDPT, inside PML4[0]:
//
//   PDPT[0]   0 .. 1 GiB     kernel: shared, points at the boot PD
//   PDPT[1]   1 .. 2 GiB     PER-PROCESS user space, private PD
//   PDPT[2]   2 .. 3 GiB     kernel: shared
//   PDPT[3]   3 .. 4 GiB     kernel: shared (MMIO lives up here -- on real
//                            hardware the framebuffer BAR usually lands
//                            around 3.5 GiB, so this must stay mapped)
//
// Each process gets its own PML4 and its own PDPT; the PDPT then points at the
// SAME kernel PDs as the boot tables, so kernel mappings never have to be
// replicated or kept in sync. Only entry 1 is private.
//
// The shared entries are copied with the USER bit cleared, and the boot PDs
// are never marked user-accessible, so ring 3 cannot reach kernel memory --
// the whole point of the exercise.
//
// Consequence, enforced in pmm_init(): physical frames in [1 GiB, 2 GiB) must
// never be allocated. The kernel reaches a frame through its identity address,
// and in a process address space that virtual range is the private user
// window, not the identity map -- so a kernel pointer into it would silently
// mean something else. Costs 1 GiB of the 4 GiB the PMM manages; on any real
// machine, and in QEMU with less than 1 GiB of RAM, nothing is actually lost.

#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITE   0x002ULL
#define PAGE_USER    0x004ULL
// "Never execute what is in this page." Only meaningful once EFER.NXE is on,
// which boot.S and the SMP trampoline both do. The processor ORs this bit
// down the whole walk, so an intermediate table must never carry it or
// everything beneath it becomes unexecutable.
#define PAGE_NX      0x8000000000000000ULL

#define USER_PDPT_INDEX 1ULL
#define USER_VIRT_BASE  0x40000000ULL   /* 1 GiB */
#define USER_VIRT_END   0x80000000ULL   /* 2 GiB */

// Create an address space: a fresh PML4 + PDPT whose kernel entries point at
// the existing kernel tables and whose user entry is empty. Returns the
// physical address of the PML4, or 0 on failure.
uint64_t paging_new_address_space(void);

// Identity-map a physical MMIO region (a device BAR) into the kernel address
// space, uncached, with 1 GiB pages. Handles BARs far above RAM. Must run
// after pmm_init. The mapping is shared into every process address space
// created afterwards.
void paging_map_mmio(uint64_t phys, uint64_t size);

// Map one 4 KiB page. Intermediate tables are allocated on demand.
// Returns 0 on success, -1 if a frame could not be allocated.
int paging_map(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags);

// Map `bytes` worth of freshly allocated, zeroed frames at vaddr.
// Returns 0 on success, -1 on failure.
int paging_map_alloc(uint64_t pml4_phys, uint64_t vaddr, uint64_t bytes, uint64_t flags);

// Translate a virtual address in the given space to a physical one, or 0
// if it is not mapped.
uint64_t paging_translate(uint64_t pml4_phys, uint64_t vaddr);

// Load this address space into CR3.
void paging_switch(uint64_t pml4_phys);

// Physical address of the address space currently in CR3.
uint64_t paging_current(void);

// Free every user page and page table of the space, then the PDPT and PML4.
// The shared kernel entries are left alone.
void paging_free_address_space(uint64_t pml4_phys);

// Change the rights on pages that are already mapped, leaving the frames
// where they are. Addresses in the range that are not mapped are skipped --
// a region built on demand is mostly holes, and they will be built with the
// new rights when they are touched.
void paging_protect(uint64_t pml4_phys, uint64_t vaddr, uint64_t bytes,
                    uint64_t flags);

// Remove the mapping and give the frame back. Returns the number of pages
// that were actually there.
int paging_unmap(uint64_t pml4_phys, uint64_t vaddr, uint64_t bytes);

// Remove the mapping and LEAVE the frame alone. For memory this address space
// was only borrowing -- shared segments, whose frames belong to the segment
// and not to whoever happens to have it mapped.
int paging_detach(uint64_t pml4_phys, uint64_t vaddr, uint64_t bytes);

#endif
