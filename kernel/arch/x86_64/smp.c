#include "smp.h"
#include "apic.h"
#include "idt.h"
#include "gdt.h"
#include "cpu.h"
#include "syscall.h"
#include "../../mm/pmm.h"
#include "../../mm/heap.h"
#include "../../kernel/kio.h"
#include "../../kernel/panic.h"
#include "pit.h"
#include <stddef.h>

// syscall_entry.S and this_cpu() read these by hard-coded offset -- keep
// them true.
_Static_assert(offsetof(struct percpu, kstack_top)   == 8,  "PERCPU_KSTACK");
_Static_assert(offsetof(struct percpu, user_rsp)     == 16, "PERCPU_USERRSP");
_Static_assert(offsetof(struct percpu, current_proc) == 24, "current_proc");
_Static_assert(offsetof(struct percpu, self)         == 32, "this_cpu()");

#define TRAMPOLINE_ADDR 0x8000
#define AP_STACK_SIZE   (16 * 1024)

#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

// The trampoline blob and the three words the BSP patches into its 0x8000 copy.
extern uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern uint8_t ap_tramp_cr3[], ap_tramp_stack[], ap_tramp_entry[];

// process.c: this core's way into the scheduler. Never returns.
extern void process_ap_run(void) __attribute__((noreturn));

static struct percpu cpus[MAX_CPUS];
static volatile int   online = 1;             // the BSP counts itself
static int            ncpu = 1;

// Set just before each SIPI; the freshly-woken AP adopts it as its per-CPU
// block. Bring-up is serialised (one AP at a time) so this needs no lock.
static volatile struct percpu *ap_next;

// Set once, when the scheduler starts: from then on the application
// processors run processes and take the timer tick.
static volatile int scheduling = 0;

static inline void set_gs_base(uint64_t base) {
    __asm__ volatile ("wrmsr" : : "c"(IA32_GS_BASE),
                      "a"((uint32_t)base), "d"((uint32_t)(base >> 32)));
}
// The kernel-entry paths (syscall_entry.S, isr.S) swapgs to reach the per-CPU
// block; the swap source is IA32_KERNEL_GS_BASE, which must also point at it.
static inline void set_kernel_gs_base(uint64_t base) {
    __asm__ volatile ("wrmsr" : : "c"(IA32_KERNEL_GS_BASE),
                      "a"((uint32_t)base), "d"((uint32_t)(base >> 32)));
}
static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}

int smp_cpu_count(void) { return ncpu; }

void smp_early_init(void) {
    cpus[0].cpu_index = 0;
    cpus[0].kstack_top = 0;
    cpus[0].self = &cpus[0];
    // The bootstrap core holds the kernel lock from the moment it starts:
    // everything before the scheduler is its alone. See bkl.h.
    cpus[0].bkl_depth = 1;
    set_gs_base((uint64_t)(uintptr_t)&cpus[0]);
    set_kernel_gs_base((uint64_t)(uintptr_t)&cpus[0]);
}

// --- the timer, for everybody ----------------------------------------------
//
// Only the bootstrap core has a timer interrupt: the PIT is wired to it. The
// others need the tick too -- it is how a program running on them is ever
// taken off the processor, and how a signal sent to it is ever noticed -- so
// the bootstrap core passes each one on as an interrupt to all the rest.
// Nothing to calibrate, and every core ticks at exactly the same rate.
void smp_tick_broadcast(void) {
    if (scheduling && ncpu > 1) lapic_broadcast_ipi(SMP_TICK_VECTOR);
}

// --- TLB shootdown ----------------------------------------------------------

// How many cores have answered the current shootdown. Counted by the assembly
// handler in isr.S, which is why it is a plain global.
volatile int tlb_acks;

void smp_tlb_shootdown(void) {
    // Before the scheduler starts, no other core has ever loaded a program's
    // page tables, so there is nobody to tell.
    if (!scheduling || ncpu <= 1) return;

    __atomic_store_n(&tlb_acks, 0, __ATOMIC_SEQ_CST);
    lapic_broadcast_ipi(SMP_TLB_VECTOR);

    // Every other core must have flushed before this returns: the caller is
    // about to let go of the kernel lock, after which the frames that were
    // just unmapped can be handed to somebody else.
    //
    // A core that never answers is a broken invariant -- something is holding
    // interrupts off where nothing should -- and carrying on regardless would
    // mean memory reached through a translation that is no longer true. So
    // after two seconds the machine stops, and says why.
    uint64_t give_up = pit_now_us() + 2000000;
    while (__atomic_load_n(&tlb_acks, __ATOMIC_ACQUIRE) < ncpu - 1) {
        if (pit_now_us() > give_up) panic("TLB shootdown: a core did not answer");
        __asm__ volatile ("pause");
    }
}

// --- SMP compute pool -----------------------------------------------------
//
// smp_run() splits a piece of pure computation into as many shares as there
// are cores, and whoever is free does them: the caller, and any core that is
// idle. A share is CLAIMED, not assigned -- that is the difference from the
// version before the scheduler ran on every core, when the other cores had
// nothing else to do and could each be given one.
//
// Now a core may be running a program, and the caller holds the kernel lock.
// A share handed to a busy core would wait for that core to reach its idle
// loop, which it cannot do while it needs the lock the caller is holding --
// and the caller would wait for the share. With claiming, a busy core simply
// never takes one, and the caller does whatever nobody else picked up. The
// worst case is the caller doing all of it: slow, and correct.
static void (* volatile job_fn)(int share, int nshares, void *arg);
static void * volatile job_arg;
static volatile int job_n;         // shares in this job
static volatile int job_next;      // the next share nobody has claimed
static volatile int job_done;      // shares finished
static volatile int job_active;

// Take shares until there are none left. Runs with interrupts OFF on the
// application processors: a tick taken in the middle of one would wait for
// the kernel lock, which is held by the core waiting for this very share.
static void run_shares(void) {
    for (;;) {
        int i = __atomic_fetch_add(&job_next, 1, __ATOMIC_ACQ_REL);
        if (i >= job_n) return;
        // Read after claiming: a share claimed in this job cannot outlive it,
        // because the job does not end until every share is done.
        job_fn(i, job_n, job_arg);
        __atomic_fetch_add(&job_done, 1, __ATOMIC_ACQ_REL);
    }
}

void smp_run(void (*fn)(int, int, void *), void *arg) {
    if (ncpu <= 1) { fn(0, 1, arg); return; }
    job_fn = fn;
    job_arg = arg;
    job_n = ncpu;
    job_done = 0;
    __atomic_store_n(&job_next, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&job_active, 1, __ATOMIC_SEQ_CST);
    lapic_broadcast_ipi(SMP_WAKE_VECTOR);   // idle cores out of hlt
    run_shares();                           // the caller takes shares too
    while (__atomic_load_n(&job_done, __ATOMIC_ACQUIRE) < job_n)
        __asm__ volatile ("pause");         // the ones others claimed
    __atomic_store_n(&job_active, 0, __ATOMIC_SEQ_CST);
}

// Called by an idle core, not holding the kernel lock, interrupts off.
void smp_idle_work(void) {
    if (__atomic_load_n(&job_active, __ATOMIC_ACQUIRE)) run_shares();
}

// --- per-core load (for the System Monitor) -------------------------------
#define LOAD_WINDOW 100                 // ticks per averaging window (1 s @100Hz)
static volatile uint8_t core_busy[MAX_CPUS];   // 1 while a core is doing work
static uint16_t         busy_accum[MAX_CPUS];  // busy ticks in the window so far
static uint8_t          core_load[MAX_CPUS];   // last computed load, 0..100
static int              load_ticks = 0;

void smp_set_busy(int busy) {
    int i = (int)this_cpu()->cpu_index;
    if (i > 0 && i < MAX_CPUS) core_busy[i] = (uint8_t)(busy ? 1 : 0);
}

// Sampled once per PIT tick on the BSP. It can see the APs' busy flags (shared)
// and infers the BSP's own from the idle flag.
void smp_sample_load(void) {
    extern volatile int g_cpu_idle;
    core_busy[0] = g_cpu_idle ? 0 : 1;
    for (int i = 0; i < ncpu; i++) if (core_busy[i]) busy_accum[i]++;
    if (++load_ticks >= LOAD_WINDOW) {
        for (int i = 0; i < ncpu; i++) {
            core_load[i] = (uint8_t)(busy_accum[i] * 100u / LOAD_WINDOW);
            busy_accum[i] = 0;
        }
        load_ticks = 0;
    }
}
int smp_core_load(int i) { return (i >= 0 && i < ncpu) ? core_load[i] : 0; }

// --- the application processors -------------------------------------------

// Landing pad for a freshly-booted application processor (called from the
// trampoline in 64-bit mode, on its own stack). Never returns.
void ap_entry(void) {
    struct percpu *pc = (struct percpu *)ap_next;

    // Everything the bootstrap core set up for itself during boot, this core
    // has to set up for itself now -- control registers and MSRs belong to
    // the core, not to the machine. See cpu.h for what goes wrong without
    // each of them.
    cpu_enable_caches();
    cpu_enable_sse();
    cpu_set_pat();

    set_gs_base((uint64_t)(uintptr_t)pc);
    set_kernel_gs_base((uint64_t)(uintptr_t)pc);
    gdt_load_cpu(pc->desc);                 // its own GDT, TSS and #DF stack
    idt_load();                             // the shared IDT
    syscall_init();                         // SYSCALL entry, per core
    apic_enable_local();                    // this core's LAPIC, for IPIs

    kprintf("smp: cpu %u (apic %u) online\n", pc->cpu_index, pc->apic_id);
    __sync_fetch_and_add(&online, 1);

    // Until the scheduler starts there are no processes to run. Wait for it
    // -- doing any compute-pool work that turns up in the meantime.
    for (;;) {
        __asm__ volatile ("cli");
        smp_idle_work();
        if (scheduling) break;
        __asm__ volatile ("sti; hlt");
    }
    process_ap_run();
}

void smp_start_scheduling(void) {
    if (scheduling) return;
    __atomic_store_n(&scheduling, 1, __ATOMIC_SEQ_CST);
    if (ncpu > 1) lapic_broadcast_ipi(SMP_WAKE_VECTOR);
}

// --- parallel benchmark (counts primes below n) ---------------------------
static volatile uint64_t bench_res[MAX_CPUS];

static int is_prime(uint64_t x) {
    if (x < 2) return 0;
    for (uint64_t j = 2; j * j <= x; j++) if (x % j == 0) return 0;
    return 1;
}
static void primes_band(int share, int nshares, void *arg) {
    uint64_t n = *(volatile uint64_t *)arg;
    uint64_t cnt = 0;
    for (uint64_t i = 2 + (uint64_t)share; i < n; i += (uint64_t)nshares)
        cnt += is_prime(i);
    bench_res[share] = cnt;
}

void smp_bench(uint64_t n, struct smp_result *out) {
    if (n < 2) n = 2;
    out->cores = ncpu;

    // Wall-clock timing needs the PIT to advance, so run with interrupts on.
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile ("sti");

    for (int i = 0; i < MAX_CPUS; i++) bench_res[i] = 0;
    uint64_t t0 = pit_get_ticks();
    primes_band(0, 1, &n);                  // one core
    out->ms_single = (uint32_t)((pit_get_ticks() - t0) * 10);

    for (int i = 0; i < MAX_CPUS; i++) bench_res[i] = 0;
    t0 = pit_get_ticks();
    smp_run(primes_band, &n);               // every core that is free
    out->ms_multi = (uint32_t)((pit_get_ticks() - t0) * 10);

    uint64_t total = 0;
    for (int i = 0; i < ncpu; i++) total += bench_res[i];
    out->primes = total;

    if (!(flags & 0x200)) __asm__ volatile ("cli");   // restore prior IF
}

static void udelay_spin(volatile uint32_t n) { while (n--) __asm__ volatile ("pause"); }
static void mdelay(uint64_t ms) {
    uint64_t start = pit_get_ticks();            // 100 Hz -> 10 ms/tick
    while ((pit_get_ticks() - start) * 10 < ms) __asm__ volatile ("pause");
}

int smp_init(void) {
    // The BSP's block was set up by smp_early_init; only the APIC id was not
    // known that early.
    cpus[0].apic_id = apic_bsp_id();

    int n = apic_cpu_count();
    if (n <= 1) { kprintf("smp: 1 core\n"); return 1; }

    // Copy the trampoline into its fixed low page and patch the shared params.
    pmm_reserve(TRAMPOLINE_ADDR, 0x1000);
    uint8_t *dst = (uint8_t *)(uintptr_t)TRAMPOLINE_ADDR;
    uint64_t blob = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    for (uint64_t i = 0; i < blob; i++) dst[i] = ap_trampoline_start[i];

    uint64_t off_cr3   = (uint64_t)(ap_tramp_cr3   - ap_trampoline_start);
    uint64_t off_stack = (uint64_t)(ap_tramp_stack - ap_trampoline_start);
    uint64_t off_entry = (uint64_t)(ap_tramp_entry - ap_trampoline_start);
    *(volatile uint64_t *)(dst + off_cr3)   = read_cr3();
    *(volatile uint64_t *)(dst + off_entry) = (uint64_t)(uintptr_t)ap_entry;

    const uint8_t *ids = apic_cpu_ids();
    uint32_t bsp = apic_bsp_id();
    int idx = 1;
    for (int i = 0; i < n && idx < MAX_CPUS; i++) {
        if (ids[i] == bsp) continue;

        // Everything the AP needs is allocated here, by this core, before it
        // is woken -- an AP that allocated for itself would be using the heap
        // with no lock held, at the same time as this core.
        void *stack = kmalloc(AP_STACK_SIZE);
        void *desc = gdt_prepare_cpu();
        if (!stack || !desc) {
            kprintf("smp: no memory for apic %u\n", ids[i]);
            if (stack) kfree(stack);
            continue;
        }
        uint64_t stack_top = (uint64_t)(uintptr_t)stack + AP_STACK_SIZE;

        cpus[idx].cpu_index = (uint32_t)idx;
        cpus[idx].apic_id = ids[i];
        cpus[idx].kstack_top = stack_top;
        cpus[idx].self = &cpus[idx];
        cpus[idx].bkl_depth = 0;
        cpus[idx].desc = desc;
        ap_next = &cpus[idx];
        *(volatile uint64_t *)(dst + off_stack) = stack_top;

        int before = online;
        // INIT, then two Startup IPIs (the classic universal sequence).
        lapic_send_init(ids[i]);
        mdelay(10);
        lapic_send_sipi(ids[i], TRAMPOLINE_ADDR >> 12);
        udelay_spin(200000);
        lapic_send_sipi(ids[i], TRAMPOLINE_ADDR >> 12);

        // Wait up to ~100 ms for the AP to report in.
        uint64_t deadline = pit_get_ticks() + 10;
        while (online == before && pit_get_ticks() < deadline) __asm__ volatile ("pause");
        if (online == before) kprintf("smp: apic %u did not start\n", ids[i]);
        else idx++;
    }

    ncpu = online;
    kprintf("smp: %d/%d cores online\n", ncpu, n);
    return ncpu;
}
