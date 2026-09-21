#include "smp.h"
#include "apic.h"
#include "idt.h"
#include "../../mm/pmm.h"
#include "../../mm/heap.h"
#include "../../kernel/kio.h"
#include "pit.h"
#include <stddef.h>

#define SMP_WAKE_VECTOR 240

// syscall_entry.S / isr.S read these by hard-coded offset -- keep them true.
_Static_assert(offsetof(struct percpu, kstack_top) == 8,  "PERCPU_KSTACK");
_Static_assert(offsetof(struct percpu, user_rsp)   == 16, "PERCPU_USERRSP");

#define MAX_CPUS 32
#define TRAMPOLINE_ADDR 0x8000
#define AP_STACK_SIZE   (16 * 1024)

#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

// The trampoline blob and the three words the BSP patches into its 0x8000 copy.
extern uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern uint8_t ap_tramp_cr3[], ap_tramp_stack[], ap_tramp_entry[];

static struct percpu cpus[MAX_CPUS];
static volatile int   online = 1;             // the BSP counts itself
static int            ncpu = 1;

// Set just before each SIPI; the freshly-woken AP adopts it as its per-CPU
// block. Bring-up is serialised (one AP at a time) so this needs no lock.
static volatile struct percpu *ap_next;

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
static inline uint64_t get_gs_base(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(IA32_GS_BASE));
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}

struct percpu *this_cpu(void) { return (struct percpu *)get_gs_base(); }
int smp_cpu_count(void) { return ncpu; }

// --- SMP compute pool -----------------------------------------------------
// The APs are a pool of kernel-mode workers. smp_run() publishes a task, wakes
// them with an IPI, runs core 0's share on the BSP, and waits at the barrier.
// A core's share is selected by (core, ncores) inside the task -- a strided
// partition of the work.
static void (* volatile smp_fn)(int core, int ncores, void *arg);
static void * volatile smp_arg;
static volatile int smp_gen = 0;      // bumped once per dispatch
static volatile int smp_done = 0;     // APs that finished the current gen
static volatile int smp_ncores = 1;

// The idle loop each application processor runs: sleep in hlt until the wake
// IPI, then run the current task if a new one was published. cli/(sti;hlt)
// closes the lost-wakeup window.
// --- per-core load (for the System Monitor) -------------------------------
#define LOAD_WINDOW 100                 // ticks per averaging window (1 s @100Hz)
static volatile uint8_t core_busy[MAX_CPUS];   // 1 while a core is doing work
static uint16_t         busy_accum[MAX_CPUS];  // busy ticks in the window so far
static uint8_t          core_load[MAX_CPUS];   // last computed load, 0..100
static int              load_ticks = 0;

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

static void ap_worker(uint32_t core) {
    int last = smp_gen;
    for (;;) {
        __asm__ volatile ("cli");
        if (smp_gen != last) {
            last = smp_gen;
            __asm__ volatile ("sti");
            core_busy[core] = 1;
            if (smp_fn) smp_fn((int)core, smp_ncores, smp_arg);
            core_busy[core] = 0;
            __sync_fetch_and_add(&smp_done, 1);
        } else {
            __asm__ volatile ("sti; hlt");
        }
    }
}

// Run `fn` on every core (BSP + APs) and return once all have finished.
void smp_run(void (*fn)(int, int, void *), void *arg) {
    if (ncpu <= 1) { fn(0, 1, arg); return; }
    smp_fn = fn; smp_arg = arg; smp_ncores = ncpu; smp_done = 0;
    __sync_synchronize();
    smp_gen++;                              // publish the task
    lapic_broadcast_ipi(SMP_WAKE_VECTOR);   // wake the APs out of hlt
    fn(0, ncpu, arg);                       // BSP runs core 0's share
    // Barrier, with a safety timeout so a wedged core degrades to a slow result
    // rather than hanging the machine.
    uint64_t deadline = pit_get_ticks() + 500;   // ~5 s
    while (smp_done < ncpu - 1 && pit_get_ticks() < deadline) __asm__ volatile ("pause");
    __sync_synchronize();
}

// Landing pad for a freshly-booted application processor (called from the
// trampoline in 64-bit mode, on its own stack). Never returns.
void ap_entry(void) {
    struct percpu *pc = (struct percpu *)ap_next;
    set_gs_base((uint64_t)(uintptr_t)pc);
    set_kernel_gs_base((uint64_t)(uintptr_t)pc);
    apic_enable_local();                    // enable THIS core's LAPIC (to get IPIs)
    idt_load();                             // share the BSP's IDT (for the wake IPI)
    kprintf("smp: cpu %u (apic %u) online\n", pc->cpu_index, pc->apic_id);
    __sync_fetch_and_add(&online, 1);
    ap_worker(pc->cpu_index);               // become a compute worker
}

// --- parallel benchmark (counts primes below n) ---------------------------
static volatile uint64_t bench_res[MAX_CPUS];

static int is_prime(uint64_t x) {
    if (x < 2) return 0;
    for (uint64_t j = 2; j * j <= x; j++) if (x % j == 0) return 0;
    return 1;
}
static void primes_band(int core, int ncores, void *arg) {
    uint64_t n = *(volatile uint64_t *)arg;
    uint64_t cnt = 0;
    for (uint64_t i = 2 + (uint64_t)core; i < n; i += (uint64_t)ncores)
        cnt += is_prime(i);
    bench_res[core] = cnt;
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
    smp_run(primes_band, &n);               // all cores
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
    // BSP is cpu 0; give it a per-CPU block and GS base too.
    cpus[0].cpu_index = 0;
    cpus[0].apic_id = apic_bsp_id();
    cpus[0].kstack_top = 0;
    set_gs_base((uint64_t)(uintptr_t)&cpus[0]);
    set_kernel_gs_base((uint64_t)(uintptr_t)&cpus[0]);

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

        void *stack = kmalloc(AP_STACK_SIZE);
        if (!stack) { kprintf("smp: no stack for apic %u\n", ids[i]); continue; }
        uint64_t stack_top = (uint64_t)(uintptr_t)stack + AP_STACK_SIZE;

        cpus[idx].cpu_index = (uint32_t)idx;
        cpus[idx].apic_id = ids[i];
        cpus[idx].kstack_top = stack_top;
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
