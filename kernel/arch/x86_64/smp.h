#ifndef SMP_H
#define SMP_H

#include <stdint.h>

struct process;

// One of these per logical core, reached through the GS base. The offsets of
// kstack_top / user_rsp are baked into syscall_entry.S (PERCPU_KSTACK=8,
// PERCPU_USERRSP=16) -- keep those two first fields fixed.
struct percpu {
    uint32_t cpu_index;    // +0   0 = BSP, 1.. = APs
    uint32_t apic_id;      // +4
    uint64_t kstack_top;   // +8   kernel stack of the process running on THIS core
    uint64_t user_rsp;     // +16  syscall scratch (parked user rsp)
    struct process *current_proc;  // +24  the process this core is running (per-CPU)
};

// Wake every application processor listed in the MADT. Call after apic_init(),
// heap_init() and gdt_init(), with interrupts enabled. Returns the number of
// cores online, including the bootstrap processor.
int  smp_init(void);
int  smp_cpu_count(void);

// The per-CPU block of the core this runs on (via the GS base MSR).
struct percpu *this_cpu(void);

// Run `fn(core, ncores, arg)` on every online core and block until all finish
// (a strided-partition parallel-for over the compute pool).
void smp_run(void (*fn)(int core, int ncores, void *arg), void *arg);

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

