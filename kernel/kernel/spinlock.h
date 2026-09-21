#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

// A test-and-set spinlock. On a single core it is essentially free (no
// contention); its real job is to make shared kernel structures safe once
// several cores -- or an IRQ handler and mainline code -- touch them at once.
//
// Use the *_irqsave variants for any structure that an interrupt handler can
// also reach (the physical allocator, the heap, device queues): they disable
// interrupts on the current core first, so a handler cannot deadlock against a
// lock the code it interrupted already holds.

typedef struct { volatile uint32_t locked; } spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1))
        while (l->locked) __asm__ volatile ("pause");   // spin read-only until free
}

static inline void spin_unlock(spinlock_t *l) {
    __sync_lock_release(&l->locked);
}

// Disable interrupts on this core, take the lock, and return the previous
// RFLAGS so the caller can restore the exact interrupt state afterwards.
static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    spin_unlock(l);
    __asm__ volatile ("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

#endif
