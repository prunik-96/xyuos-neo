// Per-process address spaces. See paging.h for the layout decision.
//
// Page tables are manipulated through their physical addresses directly: the
// kernel identity-maps the first 4 GiB, and the PMM only hands out frames from
// that range, so physical == virtual for anything the kernel touches here.

#include "paging.h"
#include "pmm.h"
#include "../kernel/kio.h"

#define ENTRIES 512
#define ADDR_MASK 0x000FFFFFFFFFF000ULL

extern uint64_t p4_table[];   // the boot PML4
extern uint64_t p3_table[];   // the boot PDPT: 4 entries covering the low 4 GiB

static uint64_t *phys_to_ptr(uint64_t phys) {
    return (uint64_t *)(uintptr_t)phys;   // identity mapped
}

static void zero_table(uint64_t *t) {
    for (int i = 0; i < ENTRIES; i++) t[i] = 0;
}

static uint64_t idx_pml4(uint64_t v) { return (v >> 39) & 0x1FF; }
static uint64_t idx_pdpt(uint64_t v) { return (v >> 30) & 0x1FF; }
static uint64_t idx_pd(uint64_t v)   { return (v >> 21) & 0x1FF; }
static uint64_t idx_pt(uint64_t v)   { return (v >> 12) & 0x1FF; }

uint64_t paging_new_address_space(void) {
    uint64_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) return 0;

    uint64_t pdpt_phys = pmm_alloc_frame();
    if (!pdpt_phys) {
        pmm_free_frame(pml4_phys);
        return 0;
    }

    uint64_t *pml4 = phys_to_ptr(pml4_phys);
    uint64_t *pdpt = phys_to_ptr(pdpt_phys);
    zero_table(pml4);
    zero_table(pdpt);

    // The PML4 entry must allow user access, because the CPU ANDs U/S down the
    // whole walk and the user window hangs off it. Isolation is enforced one
    // level lower: the kernel PDPT entries below keep USER cleared.
    pml4[0] = pdpt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;

    // Point at the very same kernel PDPT entries the boot tables use -- shared,
    // so kernel mappings need no replication. USER is stripped on the way in.
    // ALL entries except the user window are copied, not just the low four: on
    // real hardware the framebuffer/MMIO can sit above 4 GiB, mapped by boot as
    // 1 GiB pages in the high PDPT entries, and the WM renders to it even while
    // a process's address space is loaded (the idle path). Miss those and that
    // render faults.
    for (uint64_t i = 0; i < 512; i++) {
        if (i == USER_PDPT_INDEX) continue;
        if (p3_table[i] & PAGE_PRESENT) pdpt[i] = p3_table[i] & ~PAGE_USER;
    }
    // pdpt[USER_PDPT_INDEX] stays 0: the process's own PD is created lazily by
    // the first paging_map() into the user window.

    // Share the kernel's HIGHER-HALF PML4 entries too. Device BARs on real
    // hardware sit very high (the xHCI on q35 is at 768 GiB = PML4[1]), mapped
    // by paging_map_mmio into the boot PML4. xhci_poll runs from the timer IRQ
    // under whatever address space is current, so these must be visible in
    // every process space, or that poll faults. PML4[0] is handled above via
    // its own PDPT.
    for (uint64_t i = 1; i < 512; i++) {
        if (p4_table[i] & PAGE_PRESENT) pml4[i] = p4_table[i];
    }

    return pml4_phys;
}

// Identity-map a physical MMIO region into the KERNEL (boot) address space with
// 1 GiB uncached pages. For device BARs, which can sit far above RAM (768 GiB
// on q35, potentially higher on real hardware). Allocates a PDPT for the PML4
// slot on first use. Must run after pmm_init. Idempotent per 1 GiB page.
void paging_map_mmio(uint64_t phys, uint64_t size) {
    uint64_t start = phys & ~0x3FFFFFFFULL;               // 1 GiB down
    uint64_t end   = (phys + size + 0x3FFFFFFFULL) & ~0x3FFFFFFFULL;

    for (uint64_t a = start; a < end; a += 0x40000000ULL) {
        uint64_t pi = idx_pml4(a);
        uint64_t di = idx_pdpt(a);

        uint64_t *pdpt;
        if (!(p4_table[pi] & PAGE_PRESENT)) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) return;
            zero_table(phys_to_ptr(frame));
            p4_table[pi] = frame | PAGE_PRESENT | PAGE_WRITE;
        }
        pdpt = phys_to_ptr(p4_table[pi] & ADDR_MASK);

        // 1 GiB page: present | write | PS(0x80) | PCD(0x10, uncached).
        pdpt[di] = a | PAGE_PRESENT | PAGE_WRITE | 0x80ULL | 0x10ULL;
    }

    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

// Fetch the next-level table for `vaddr`, creating it when `create` is set.
// Returns NULL if absent (or if a frame could not be allocated).
static uint64_t *next_level(uint64_t *table, uint64_t index, int create, uint64_t flags) {
    if (!(table[index] & PAGE_PRESENT)) {
        if (!create) return 0;
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return 0;
        zero_table(phys_to_ptr(frame));
        table[index] = frame | PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);
    } else {
        // Widen permissions if a shallower mapping was created first: the CPU
        // ANDs U/S and R/W across the whole walk, so an intermediate entry
        // that is missing USER would silently veto a user mapping below it.
        table[index] |= (flags & (PAGE_USER | PAGE_WRITE));
    }
    return phys_to_ptr(table[index] & ADDR_MASK);
}

int paging_map(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t *pml4 = phys_to_ptr(pml4_phys);

    uint64_t *pdpt = next_level(pml4, idx_pml4(vaddr), 1, flags);
    if (!pdpt) return -1;
    uint64_t *pd = next_level(pdpt, idx_pdpt(vaddr), 1, flags);
    if (!pd) return -1;
    uint64_t *pt = next_level(pd, idx_pd(vaddr), 1, flags);
    if (!pt) return -1;

    pt[idx_pt(vaddr)] = (paddr & ADDR_MASK) | (flags & 0xFFF)
                      | (flags & PAGE_NX) | PAGE_PRESENT;
    return 0;
}

int paging_map_alloc(uint64_t pml4_phys, uint64_t vaddr, uint64_t bytes, uint64_t flags) {
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end = (vaddr + bytes + 0xFFF) & ~0xFFFULL;

    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        if (paging_translate(pml4_phys, v)) continue;   // already mapped
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return -1;
        uint8_t *p = (uint8_t *)(uintptr_t)frame;
        for (uint64_t i = 0; i < PAGE_SIZE; i++) p[i] = 0;
        if (paging_map(pml4_phys, v, frame, flags) != 0) {
            pmm_free_frame(frame);
            return -1;
        }
    }
    return 0;
}

uint64_t paging_translate(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pml4 = phys_to_ptr(pml4_phys);

    uint64_t *pdpt = next_level(pml4, idx_pml4(vaddr), 0, 0);
    if (!pdpt) return 0;
    uint64_t *pd = next_level(pdpt, idx_pdpt(vaddr), 0, 0);
    if (!pd) return 0;

    // A 2 MiB page (PS bit) ends the walk one level early.
    uint64_t pde = pd[idx_pd(vaddr)];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (pde & 0x80) return (pde & ADDR_MASK) + (vaddr & 0x1FFFFF);

    uint64_t *pt = phys_to_ptr(pde & ADDR_MASK);
    uint64_t pte = pt[idx_pt(vaddr)];
    if (!(pte & PAGE_PRESENT)) return 0;
    return (pte & ADDR_MASK) + (vaddr & 0xFFF);
}

void paging_switch(uint64_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t paging_current(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ADDR_MASK;
}

void paging_free_address_space(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    uint64_t *pml4 = phys_to_ptr(pml4_phys);
    if (!(pml4[0] & PAGE_PRESENT)) {
        pmm_free_frame(pml4_phys);
        return;
    }

    uint64_t pdpt_phys = pml4[0] & ADDR_MASK;
    uint64_t *pdpt = phys_to_ptr(pdpt_phys);

    // Only the user entry is ours to free. The other PDPT entries point at the
    // boot PDs, shared with the kernel and with every other address space --
    // freeing those would hand live kernel page tables back to the allocator.
    uint64_t pde_entry = pdpt[USER_PDPT_INDEX];
    if (pde_entry & PAGE_PRESENT) {
        uint64_t *pd = phys_to_ptr(pde_entry & ADDR_MASK);

        for (uint64_t k = 0; k < ENTRIES; k++) {
            if (!(pd[k] & PAGE_PRESENT)) continue;
            if (pd[k] & 0x80) continue;          // 2 MiB page, no PT below
            uint64_t *pt = phys_to_ptr(pd[k] & ADDR_MASK);

            for (uint64_t l = 0; l < ENTRIES; l++)
                if (pt[l] & PAGE_PRESENT) pmm_free_frame(pt[l] & ADDR_MASK);

            pmm_free_frame(pd[k] & ADDR_MASK);
        }
        pmm_free_frame(pde_entry & ADDR_MASK);
    }

    pmm_free_frame(pdpt_phys);
    pmm_free_frame(pml4_phys);
}

/* --- changing a mapping after the fact ------------------------------------ */

static void invlpg(uint64_t v) {
    __asm__ volatile ("invlpg (%0)" :: "r"((void *)(uintptr_t)v) : "memory");
}

/* Walk to the page table entry for `v`, or NULL. Never creates anything:
 * both callers below are only interested in pages that already exist. */
static uint64_t *pte_of(uint64_t pml4_phys, uint64_t v) {
    uint64_t *pml4 = phys_to_ptr(pml4_phys);
    uint64_t *pdpt = next_level(pml4, idx_pml4(v), 0, 0);
    if (!pdpt) return 0;
    uint64_t *pd = next_level(pdpt, idx_pdpt(v), 0, 0);
    if (!pd) return 0;
    uint64_t pde = pd[idx_pd(v)];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (pde & 0x80) return 0;            /* a 2 MiB page has no table below */
    uint64_t *pt = phys_to_ptr(pde & ADDR_MASK);
    if (!(pt[idx_pt(v)] & PAGE_PRESENT)) return 0;
    return &pt[idx_pt(v)];
}

void paging_protect(uint64_t pml4_phys, uint64_t vaddr, uint64_t bytes,
                    uint64_t flags) {
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end = (vaddr + bytes + 0xFFF) & ~0xFFFULL;
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        uint64_t *e = pte_of(pml4_phys, v);
        if (!e) continue;
        *e = (*e & ADDR_MASK) | (flags & 0xFFF) | (flags & PAGE_NX)
           | PAGE_PRESENT;
        invlpg(v);
    }
}

int paging_unmap(uint64_t pml4_phys, uint64_t vaddr, uint64_t bytes) {
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end = (vaddr + bytes + 0xFFF) & ~0xFFFULL;
    int n = 0;
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        uint64_t *e = pte_of(pml4_phys, v);
        if (!e) continue;
        uint64_t frame = *e & ADDR_MASK;
        *e = 0;
        invlpg(v);
        pmm_free_frame(frame);
        n++;
    }
    /* The page tables themselves are left standing. They are one frame per
     * two megabytes of address space and a program that unmaps something
     * usually maps something else nearby; walking them to find empty ones
     * would cost more than it saves. */
    return n;
}
