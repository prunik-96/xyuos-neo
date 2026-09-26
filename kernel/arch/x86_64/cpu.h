#ifndef CPU_H
#define CPU_H

// What each core has to set up for itself.
//
// Control registers and MSRs belong to the core, not to the machine. The
// bootstrap core sets its own during boot, and every application processor
// comes out of reset with its own at their reset values -- which is to say
// wrong in ways that do not show until something needs them. These are the
// ones that matter here, in one place, so the cores cannot drift apart.

// SSE and the FPU: CR0.EM off, CR0.MP on, CR4.OSFXSR and CR4.OSXMMEXCPT on.
// Without them FXSAVE -- which every context switch does -- is an invalid
// instruction, and so is every floating-point instruction a program runs.
void cpu_enable_sse(void);

// The page attribute table, with entry 4 made Write-Combining for the
// framebuffer. Every core must agree: two cores mapping the same memory with
// different types is undefined behaviour in the processor's own terms.
void cpu_set_pat(void);

// Caches on. An application processor leaves reset with CR0.CD and CR0.NW
// set -- caching disabled -- and nothing on the way to long mode changes
// that. A core running uncached works, correctly and many times slower.
void cpu_enable_caches(void);

#endif
