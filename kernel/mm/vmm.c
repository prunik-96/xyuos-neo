#include "vmm.h"
#include "paging.h"
#include "pmm.h"
#include "../kernel/process.h"
#include "heap.h"

#define PAGE 4096ULL

int vmm_user_range_ok(uint64_t addr, uint64_t len) {
    if (addr < USER_VIRT_BASE) return 0;
    if (addr >= USER_VIRT_END) return 0;
    // Checked as a subtraction rather than `addr + len > END` so that a huge
    // length cannot wrap the sum back into the valid window.
    if (len > USER_VIRT_END - addr) return 0;
    return 1;
}

/* --- the memory of a process ---------------------------------------------- */

addr_space_t *vmm_space_new(void) {
    addr_space_t *as = kmalloc(sizeof *as);
    if (!as) return 0;

    uint8_t *q = (uint8_t *)as;
    for (uint64_t i = 0; i < sizeof *as; i++) q[i] = 0;

    as->pml4 = paging_new_address_space();
    if (!as->pml4) { kfree(as); return 0; }

    as->brk = USER_HEAP_BASE;
    as->refs = 1;
    return as;
}

void vmm_space_ref(addr_space_t *as) {
    if (as) as->refs++;
}

void vmm_space_unref(addr_space_t *as) {
    if (!as) return;
    if (--as->refs > 0) return;
    paging_free_address_space(as->pml4);
    kfree(as);
}

/* --- the page flags a protection asks for --------------------------------- */

static uint64_t flags_of(uint32_t prot) {
    uint64_t f = PAGE_PRESENT | PAGE_USER;
    if (prot & VM_WRITE) f |= PAGE_WRITE;
    /* Anything not asked to be executable is marked never-executable. That is
     * the whole of W^X from this side: a program gets pages it can write or
     * pages it can run, and has to say which by asking. */
    if (!(prot & VM_EXEC)) f |= PAGE_NX;
    return f;
}

/* --- the list ------------------------------------------------------------- */

static vm_region_t *region_at(process_t *p, uint64_t addr) {
    for (int i = 0; i < VM_REGIONS_MAX; i++) {
        vm_region_t *r = &p->as->vm[i];
        if (r->base && addr >= r->base && addr < r->base + r->len) return r;
    }
    return 0;
}

static vm_region_t *region_exact(process_t *p, uint64_t addr, uint64_t len) {
    for (int i = 0; i < VM_REGIONS_MAX; i++) {
        vm_region_t *r = &p->as->vm[i];
        if (r->base == addr && r->len == len) return r;
    }
    return 0;
}

uint64_t vmm_mmap_floor(void) {
    process_t *p = process_current();
    if (!p || !p->as) return USER_MMAP_TOP;
    uint64_t low = USER_MMAP_TOP;
    for (int i = 0; i < VM_REGIONS_MAX; i++)
        if (p->as->vm[i].base && p->as->vm[i].base < low)
            low = p->as->vm[i].base;
    return low;
}

/* Where a mapping of `len` bytes can go: as high as possible, walking down
 * past anything already there. Stops before the break, which is the heap's
 * to grow into. */
static uint64_t place(process_t *p, uint64_t len) {
    uint64_t top = USER_MMAP_TOP;
    for (int guard = 0; guard < VM_REGIONS_MAX + 1; guard++) {
        if (top < len) return 0;
        uint64_t cand = (top - len) & ~(PAGE - 1);
        /* A page of daylight above the break, so that running off the end of
         * the heap lands on nothing rather than on somebody's mapping. */
        if (cand < p->as->brk + PAGE) return 0;

        vm_region_t *hit = 0;
        for (int i = 0; i < VM_REGIONS_MAX; i++) {
            vm_region_t *r = &p->as->vm[i];
            if (!r->base) continue;
            if (cand < r->base + r->len && r->base < cand + len) { hit = r; break; }
        }
        if (!hit) return cand;
        top = hit->base;                    /* try again below that one */
    }
    return 0;
}

uint64_t vmm_mmap(uint64_t len, uint32_t prot) {
    process_t *p = process_current();
    if (!p || !p->as || len == 0) return 0;

    len = (len + PAGE - 1) & ~(PAGE - 1);

    vm_region_t *slot = 0;
    for (int i = 0; i < VM_REGIONS_MAX; i++)
        if (!p->as->vm[i].base) { slot = &p->as->vm[i]; break; }
    if (!slot) return 0;                    /* out of slots, not of memory */

    uint64_t at = place(p, len);
    if (!at) return 0;

    slot->base = at;
    slot->len = len;
    slot->prot = prot;
    /* No pages. They are built by vmm_fault() as the program reaches them,
     * which is the point of asking for a big mapping and using a little. */
    return at;
}

int vmm_munmap(uint64_t addr, uint64_t len) {
    process_t *p = process_current();
    if (!p || !p->as) return -1;

    len = (len + PAGE - 1) & ~(PAGE - 1);

    /* Whole mappings only. Splitting one in half is a real thing for mmap to
     * do and nothing here needs it; refusing is honest, and quietly unmapping
     * the wrong amount would not be. */
    vm_region_t *r = region_exact(p, addr, len);
    if (!r) return -1;

    paging_unmap(p->pml4, r->base, r->len);
    r->base = 0;
    r->len = 0;
    return 0;
}

int vmm_mprotect(uint64_t addr, uint64_t len, uint32_t prot) {
    process_t *p = process_current();
    if (!p || !p->as) return -1;

    len = (len + PAGE - 1) & ~(PAGE - 1);
    vm_region_t *r = region_exact(p, addr, len);
    if (!r) return -1;

    r->prot = prot;
    /* The pages that exist change now; the ones that do not will be built
     * with the new rights when they are touched. */
    paging_protect(p->pml4, r->base, r->len, flags_of(prot));
    return 0;
}

/* --- pages that appear when they are touched -------------------------------
 *
 * Three parts of a process are declared rather than built: the stack, which
 * is eight megabytes of address space that almost nothing ever fills, the
 * heap below the break, which malloc grows a quarter of a megabyte at a time
 * whether or not the program is about to use it, and every mapping. Building
 * any of them eagerly costs frames for memory that is never read.
 *
 * Everything outside them still ends the program, which is the point: the
 * gap between the top of the heap and the lowest mapping is a guard, and so
 * is the page below the stack.
 */
int vmm_fault(uint64_t addr, uint64_t err) {
    process_t *p = process_current();
    if (!p || !p->as || !p->pml4) return 0;
    if (addr < USER_VIRT_BASE || addr >= USER_VIRT_END) return 0;

    /* The page is there and the processor would not let the program have it:
     * a write to something read-only, or an instruction fetched out of a
     * page marked never-execute. There is nothing to build -- this is the
     * program being stopped from doing what it was told it could not. */
    if (err & PF_PRESENT) return 0;

    int write = (err & PF_WRITE) != 0;

    uint64_t page = addr & ~(PAGE - 1);
    uint64_t flags;

    if (page >= USER_STACK_BASE && page < USER_STACK_TOP) {
        /* Nothing here has ever run code from the stack, and there is no
         * reason to allow it. */
        flags = PAGE_PRESENT | PAGE_USER | PAGE_WRITE | PAGE_NX;
    } else if (page >= USER_HEAP_BASE && page < p->as->brk) {
        /* The heap keeps the right to be executed, deliberately.
         *
         * libc promises that memory which is not a mapping allows
         * everything -- mprotect on a malloc'd pointer reports success
         * because there is genuinely nothing to change -- and taking that
         * away here would make the promise a lie. A program that wants
         * writable memory and runnable memory kept apart asks for mappings
         * and says what each of them is for; that is where W^X lives.
         *
         * Making the heap non-executable too is a real improvement and a
         * separate change: it needs a way for a program to ask for heap it
         * can run, and every program on the disc retested against it. */
        flags = PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
    } else {
        vm_region_t *r = region_at(p, page);
        if (!r) return 0;
        /* Asking of a mapping what it was not asked to allow is the
         * program's mistake, not a page that is missing. */
        if (write && !(r->prot & VM_WRITE)) return 0;
        if ((err & PF_FETCH) && !(r->prot & VM_EXEC)) return 0;
        flags = flags_of(r->prot);
    }

    /* Two faults on the same page can reach here -- the second one has
     * nothing to do. */
    if (paging_translate(p->pml4, page)) return 1;

    uint64_t frame = pmm_alloc_frame();
    if (!frame) return 0;         /* out of memory: let it die honestly */

    /* Zeroed, because the frame was somebody else's a moment ago and a
     * program must never be handed another program's leavings. */
    uint8_t *q = (uint8_t *)(uintptr_t)frame;
    for (uint64_t i = 0; i < PAGE; i++) q[i] = 0;

    if (paging_map(p->pml4, page, frame, flags) != 0) {
        pmm_free_frame(frame);
        return 0;
    }

    /* The processor is allowed to remember that this page was NOT there.
     * Saying otherwise now is what lets the faulting instruction succeed
     * when it is tried again. */
    __asm__ volatile ("invlpg (%0)" :: "r"((void *)(uintptr_t)page) : "memory");
    return 1;
}
