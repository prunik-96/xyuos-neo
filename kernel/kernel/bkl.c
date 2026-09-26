// The big kernel lock. See bkl.h for what it is for and the rules it keeps.

#include "bkl.h"
#include "panic.h"
#include "../arch/x86_64/smp.h"

// The core holding the lock, or -1. The bootstrap core has it from the first
// instruction: everything before the scheduler starts is its alone, and it
// lets go for the first time when it goes idle.
static volatile int owner = 0;

int bkl_owner_cpu(void) { return owner; }

static inline uint64_t read_flags(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0" : "=r"(f) :: "memory");
    return f;
}

void bkl_enter(void) {
    struct percpu *c = this_cpu();
    int me = (int)c->cpu_index;

    if (__atomic_load_n(&owner, __ATOMIC_RELAXED) == me) {
        c->bkl_depth++;
        return;
    }

    uint64_t flags = read_flags();
    for (;;) {
        int expect = -1;
        if (__atomic_compare_exchange_n(&owner, &expect, me, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        // Wait with interrupts open. `sti` holds interrupts off for exactly
        // one more instruction, so this is a window of one `pause` in which
        // anything pending -- above all a TLB shootdown from the core that
        // has the lock -- is taken. Without it, a core stuck here could be
        // the one the lock holder is waiting for.
        //
        // The window is re-opened each time round rather than left open,
        // because some callers (the system call entry) arrive with interrupts
        // off and must get them back that way.
        while (__atomic_load_n(&owner, __ATOMIC_RELAXED) != -1)
            __asm__ volatile ("sti; pause; cli" ::: "memory");
    }
    if (flags & 0x200) __asm__ volatile ("sti" ::: "memory");

    c->bkl_depth = 1;
}

int bkl_try_enter(void) {
    struct percpu *c = this_cpu();
    int me = (int)c->cpu_index;
    if (__atomic_load_n(&owner, __ATOMIC_RELAXED) == me) {
        c->bkl_depth++;
        return 1;
    }
    int expect = -1;
    if (!__atomic_compare_exchange_n(&owner, &expect, me, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    c->bkl_depth = 1;
    return 1;
}

void bkl_exit(void) {
    struct percpu *c = this_cpu();
    if (c->bkl_depth <= 0)
        panic("bkl: released by a core that does not hold it");
    if (--c->bkl_depth == 0)
        __atomic_store_n(&owner, -1, __ATOMIC_RELEASE);
}

void bkl_exit_to_user(void) {
    struct percpu *c = this_cpu();
    if (c->bkl_depth != 1)
        panic("bkl: leaving for user mode at the wrong depth");
    c->bkl_depth = 0;
    __atomic_store_n(&owner, -1, __ATOMIC_RELEASE);
}

void bkl_wait_interrupt(void) {
    struct percpu *c = this_cpu();
    int depth = c->bkl_depth;
    c->bkl_depth = 0;
    __atomic_store_n(&owner, -1, __ATOMIC_RELEASE);

    __asm__ volatile ("sti; hlt; cli" ::: "memory");

    // Back, possibly after other cores have been through the kernel. Same
    // core -- nothing here can move a caller to another one -- so `c` is
    // still ours.
    bkl_enter();
    c->bkl_depth = depth;
}
