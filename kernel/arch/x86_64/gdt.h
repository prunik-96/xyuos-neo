#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Layout is fixed by the SYSCALL/SYSRET STAR MSR convention: with
// STAR[47:32]=kernel CS, kernel SS must sit at CS+8. With STAR[63:48]=0x18,
// SYSRETQ loads CS from 0x18+16=0x28 and SS from 0x18+8=0x20 -- hence the
// dummy slot at 0x18 and user data/code at 0x20/0x28 (not swapped).
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   (0x20 | 3)
#define GDT_USER_CODE   (0x28 | 3)
#define GDT_TSS         0x30

void gdt_init(void);
void tss_set_kernel_stack(uint64_t rsp0);
uint64_t gdt_entry_raw(int index);   // panic diagnostics

#endif
