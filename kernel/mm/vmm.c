#include "vmm.h"
#include "paging.h"
#include "pmm.h"
#include "../kernel/process.h"

int vmm_user_range_ok(uint64_t addr, uint64_t len) {
    if (addr < USER_VIRT_BASE) return 0;
    if (addr >= USER_VIRT_END) return 0;
    // Checked as a subtraction rather than `addr + len > END` so that a huge
    // length cannot wrap the sum back into the valid window.
    if (len > USER_VIRT_END - addr) return 0;
    return 1;
}

/* --- pages that appear when they are touched -------------------------------
 *
 * Two regions of a process are declared rather than built: the stack, which
 * is eight megabytes of address space that almost nothing ever fills, and the
 * heap below the break, which malloc grows a quarter of a megabyte at a time
 * whether or not the program is about to use it. Building either eagerly
 * costs frames for memory that is never read.
 *
 * So they are simply not mapped, and the page fault handler maps a page the
 * first time one is touched. Everything outside those two ranges still ends
 * the program, which is the point: the gap between the top of the heap and
 * the bottom of the stack is a guard, and running off either into the other
 * is a fault rather than silent corruption.
 */
int vmm_fault(uint64_t addr, int write) {
    (void)write;                  /* both regions are readable and writable */

    process_t *p = process_current();
    if (!p || !p->pml4) return 0;
    if (addr < USER_VIRT_BASE || addr >= USER_VIRT_END) return 0;

    uint64_t page = addr & ~0xFFFULL;

    int in_stack = page >= USER_STACK_BASE && page < USER_STACK_TOP;
    int in_heap  = page >= USER_HEAP_BASE  && page < p->brk;
    if (!in_stack && !in_heap) return 0;

    /* Two faults on the same page can reach here -- the second one has
     * nothing to do. */
    if (paging_translate(p->pml4, page)) return 1;

    uint64_t frame = pmm_alloc_frame();
    if (!frame) return 0;         /* out of memory: let it die honestly */

    /* Zeroed, because the frame was somebody else's a moment ago and a
     * program must never be handed another program's leavings. */
    uint8_t *q = (uint8_t *)(uintptr_t)frame;
    for (uint64_t i = 0; i < 4096; i++) q[i] = 0;

    if (paging_map(p->pml4, page, frame,
                   PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        pmm_free_frame(frame);
        return 0;
    }

    /* The processor is allowed to remember that this page was NOT there.
     * Saying otherwise now is what lets the faulting instruction succeed
     * when it is tried again. */
    __asm__ volatile ("invlpg (%0)" :: "r"((void *)(uintptr_t)page) : "memory");
    return 1;
}
