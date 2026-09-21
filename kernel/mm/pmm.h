#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096ULL

void pmm_init(uint32_t multiboot_addr);
// Mark a physical range as used so the allocator never hands it out (e.g. the
// low page the SMP trampoline is copied to). Range is [phys, phys+len).
void pmm_reserve(uint64_t phys, uint64_t len);
uint64_t pmm_alloc_frame(void);
// Allocate `count` PHYSICALLY CONSECUTIVE frames and return the address of the
// first, or 0. DMA hardware walks a buffer by address, so a driver ring has to
// be one unbroken run -- taking frames one at a time and hoping they come out
// adjacent works only while the bitmap is still nearly empty, which stops
// being true the moment anything else has booted first.
uint64_t pmm_alloc_contig(uint64_t count);
void pmm_free_frame(uint64_t phys_addr);
uint64_t pmm_free_frame_count(void);
uint64_t pmm_total_frame_count(void);

#endif
