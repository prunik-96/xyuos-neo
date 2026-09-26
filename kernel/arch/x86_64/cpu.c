// What each core has to set up for itself. See cpu.h.

#include "cpu.h"
#include <stdint.h>

void cpu_enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);   // clear EM (no FPU emulation)
    cr0 |=  (1UL << 1);   // set MP
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10);  // OSFXSR | OSXMMEXCPT
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
}

// Program the PAT so a page with the PAT bit set (PCD=PWT=0) is Write-Combining.
// IA32_PAT (MSR 0x277): make entry PA4 = WC (0x01); leave the rest at reset
// defaults. No existing mapping sets the PAT bit, so nothing else is affected.
void cpu_set_pat(void) {
    uint32_t lo = 0x00070406;   // PA3..PA0 = UC, UC-, WT, WB  (reset default)
    uint32_t hi = 0x00070401;   // PA7..PA4 = UC, UC-, WT, WC  (PA4 changed to WC)
    __asm__ volatile ("wrmsr" :: "c"(0x277u), "a"(lo), "d"(hi) : "memory");
}

void cpu_enable_caches(void) {
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    if (!(cr0 & ((1UL << 30) | (1UL << 29)))) return;   // already on
    // Write back and drop whatever the caches hold before switching them on,
    // so nothing stale is found in them afterwards.
    __asm__ volatile ("wbinvd" ::: "memory");
    cr0 &= ~((1UL << 30) | (1UL << 29));                 // CD and NW off
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}
