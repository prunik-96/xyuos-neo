#ifndef BKL_H
#define BKL_H

// The big kernel lock.
//
// Programs run on every core at once. The kernel runs on one core at a time.
//
// That is the whole design, and it is deliberately the one Linux 2.0 and
// FreeBSD 4 started with. Everything in this kernel was written for one
// processor: the scheduler, the process table, the file system, the window
// manager, the keyboard queue, the pipes, the network stack. Making each of
// those safe for concurrent use is a separate piece of work per subsystem, and
// doing it by halves produces a machine that corrupts memory rarely and
// unpredictably -- the worst outcome there is. With one lock around the whole
// kernel, every one of those assumptions stays true without being touched,
// and what is gained is the part that matters most: user code, which is
// where programs spend their time, runs in parallel.
//
// The rules:
//
//   A core holds the lock whenever it is executing kernel code, apart from
//   the few instructions of an entry stub before it takes it and of an exit
//   stub after it lets go, the idle `hlt`, and two interrupt handlers that
//   touch nothing shared (the TLB shootdown and the wake-up).
//
//   It is let go of only AFTER the core has left the kernel stack it was
//   using. The lock is what stops another core resuming a process whose
//   stack this core is still standing on.
//
//   Waiting for it is done with interrupts ON. A core that holds the lock
//   may need every other core to answer a shootdown before it can continue;
//   a core deaf to interrupts while it waits would never answer, and the two
//   would wait for each other for ever.
//
//   It nests. A fault taken in the middle of a system call enters it again,
//   and so does an interrupt that arrives while kernel code has interrupts
//   enabled. Each core counts its own depth.

// Take the lock, or take it once more if this core already has it.
void bkl_enter(void);

// Let go of it once. The last one out releases it.
void bkl_exit(void);

// Let go of it on the way back to ring 3, from the context-switch paths. The
// depth must be exactly one there -- anything else means an entry and an exit
// do not match somewhere, and continuing would hand the kernel to two cores
// at once. So it stops the machine instead.
void bkl_exit_to_user(void);

// For the one place that has to wait for an interrupt while in the middle of
// a system call (the screen-off sleep). Lets go of the lock completely,
// halts until an interrupt, and takes it back to the same depth. Whatever the
// caller was relying on may have changed in between; it must look again.
void bkl_wait_interrupt(void);

// Which core holds it, or -1. For the panic screen.
int bkl_owner_cpu(void);

#endif
