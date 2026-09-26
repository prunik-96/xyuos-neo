#ifndef SMP_H
#define SMP_H

#include <stdint.h>

struct process;

#define MAX_CPUS 32

// Interrupt vectors for the cores talking to each other. The numbers are not
// arbitrary: a Local APIC holds back any interrupt whose vector is in the same
// priority class (vector / 16) as one it is still servicing, or a lower one.
//
//   TICK  -- the timer, passed on from the bootstrap core. Class 14, so that
//            while a core is still inside one tick (it may be waiting for the
//            kernel lock), a SHOOTDOWN can still get through to it.
//   WAKE  -- "look at the work queues". Class 15.
//   TLB   -- "forget your cached translations". Class 15, above everything
//            that can ever be waiting for the lock. If this could be held
//            back by a core that is itself waiting, the core that sent it
//            would wait for ever.
#define SMP_TICK_VECTOR 0xE0
#define SMP_WAKE_VECTOR 0xF0
#define SMP_TLB_VECTOR  0xF3

// One of these per logical core, reached through the GS base. The offsets of
// kstack_top / user_rsp are baked into syscall_entry.S (PERCPU_KSTACK=8,
// PERCPU_USERRSP=16), and `self` is read by this_cpu() at a fixed offset --
// keep the first six fields where they are.
struct percpu {
    uint32_t cpu_index;    // +0   0 = BSP, 1.. = APs
    uint32_t apic_id;      // +4
    uint64_t kstack_top;   // +8   kernel stack of the process running on THIS core
    uint64_t user_rsp;     // +16  syscall scratch (parked user rsp)
    struct process *current_proc;  // +24  the process this core is running
    struct percpu  *self;          // +32  this block's own address

    // How many times this core has entered the kernel lock without leaving.
    // It nests: a page fault taken in the middle of a system call enters it
    // again. See kernel/kernel/bkl.h.
    int bkl_depth;

    // Where this core's scheduler loop is: every block and every exit on this
    // core ends by jumping here, onto a stack that belongs to the core and
    // not to any process. See cpu_loop() in process.c.
    uint64_t idle_ctx[8];

    // This core's GDT and task state segment. Opaque here; gdt.c owns it.
    void *desc;

    // Set while this core's timer interrupt is waiting for the kernel lock
    // or doing its work. A tick that arrives meanwhile moves the clock and
    // leaves -- see irq_handler.
    int in_timer;
};

// The per-CPU block of the core this runs on.
//
// Read through GS rather than the GS-base MSR: rdmsr is a serialising
// instruction and this is called on every kernel entry. Valid from the first
// line of kernel_main (smp_early_init) onwards, and in kernel mode only --
// the entry stubs swapgs on the way in, so GS is always the kernel's here.
//
// NEVER keep the result across anything that can block. A process that
// blocks on one core may well be resumed on another, and the pointer it was
// holding then names somebody else's core.
static inline struct percpu *this_cpu(void) {
    struct percpu *p;
    __asm__ volatile ("mov %%gs:32, %0" : "=r"(p));
    return p;
}

// Give the bootstrap core its per-CPU block. The very first thing kernel_main
// does: everything after it, interrupt handlers included, may use this_cpu().
void smp_early_init(void);

// Wake every application processor listed in the MADT. Call after apic_init(),
// heap_init() and gdt_init(), with interrupts enabled. Returns the number of
// cores online, including the bootstrap processor.
int  smp_init(void);
int  smp_cpu_count(void);

// Split pure computation into one share per core: `fn(share, nshares, arg)`
// for share = 0..nshares-1, done by the caller and whichever cores are idle,
// and return once every share is done. The shares must touch nothing shared
// with the rest of the kernel -- they run on other cores without its lock.
void smp_run(void (*fn)(int share, int nshares, void *arg), void *arg);

// An idle core's part in the above: take shares while there are any. Called
// from the idle loop, without the kernel lock, with interrupts off.
void smp_idle_work(void);

// Start the application processors on processes. Called once, by the
// bootstrap core, when its own scheduler starts.
void smp_start_scheduling(void);

// Pass the timer tick on to every other core. From the bootstrap core's timer
// interrupt, before the kernel lock is taken.
void smp_tick_broadcast(void);

// Make every other core forget its cached translations, and wait until each
// has. Called by the holder of the kernel lock after it has removed or
// restricted a mapping in page tables that another core may be using.
void smp_tlb_shootdown(void);

// Mark THIS core as running something (1) or idle (0), for the per-core load
// in the System Monitor. The bootstrap core's comes from its idle flag.
void smp_set_busy(int busy);

// Result of the built-in parallel benchmark (counts primes below n on one core,
// then on all cores, timing each). Layout mirrored by libc's struct smp_result.
struct smp_result {
    int      cores;
    uint32_t ms_single;
    uint32_t ms_multi;
    uint64_t primes;
};
void smp_bench(uint64_t n, struct smp_result *out);

// Per-core CPU load for the System Monitor. smp_sample_load() is called once
// per PIT tick on the BSP; smp_core_load(i) returns core i's load as 0..100.
void smp_sample_load(void);
int  smp_core_load(int i);

#endif
