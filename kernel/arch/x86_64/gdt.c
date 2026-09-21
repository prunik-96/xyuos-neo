#include "gdt.h"
#include <stddef.h>

struct tss_struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// 6 normal 8-byte descriptors (null, kcode, kdata, dummy32, udata, ucode)
// followed by one 16-byte TSS descriptor -> 64 bytes total.
static uint8_t gdt[64];
static struct gdt_pointer gdtp;
static struct tss_struct tss;

// A dedicated stack for the double-fault and page-fault handlers (IST1). If a
// process overflows its 16 KiB kernel stack, the CPU can no longer push the
// exception frame onto that stack -- without an IST that turns into a triple
// fault and a silent reset. Routing #DF/#PF onto this fresh stack means we get
// a clean panic with rip/cr2 instead.
static uint8_t ist1_stack[16384] __attribute__((aligned(16)));

extern void gdt_flush(uint64_t gdt_pointer_addr);
extern void tss_flush(uint16_t selector);

static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    uint8_t *e = &gdt[index * 8];
    e[0] = limit & 0xFF;
    e[1] = (limit >> 8) & 0xFF;
    e[6] = (limit >> 16) & 0x0F;
    e[2] = base & 0xFF;
    e[3] = (base >> 8) & 0xFF;
    e[4] = (base >> 16) & 0xFF;
    e[7] = (base >> 24) & 0xFF;
    e[5] = access;
    e[6] |= (flags & 0xF0);
}

static void gdt_set_tss(int index, uint64_t base, uint32_t limit) {
    gdt_set_entry(index, (uint32_t)base, limit, 0x89, 0x00);
    uint8_t *e = &gdt[(index + 1) * 8];
    uint32_t base_upper = (uint32_t)(base >> 32);
    e[0] = base_upper & 0xFF;
    e[1] = (base_upper >> 8) & 0xFF;
    e[2] = (base_upper >> 16) & 0xFF;
    e[3] = (base_upper >> 24) & 0xFF;
    e[4] = 0;
    e[5] = 0;
    e[6] = 0;
    e[7] = 0;
}

void gdt_init(void) {
    for (int i = 0; i < 8; i++) {
        uint8_t *word_ptr = &gdt[i * 8];
        for (int j = 0; j < 8; j++) word_ptr[j] = 0;
    }

    gdt_set_entry(0, 0, 0, 0, 0);                          // null
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);               // kernel code (0x08)
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);               // kernel data (0x10)
    gdt_set_entry(3, 0, 0xFFFFF, 0x9A, 0xA0);               // dummy 32-bit user code (0x18, unused)
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);               // user data (0x20|3)
    gdt_set_entry(5, 0, 0xFFFFF, 0xFA, 0xA0);               // user code (0x28|3)

    for (int i = 0; i < (int)sizeof(tss); i++) {
        ((uint8_t *)&tss)[i] = 0;
    }
    tss.iomap_base = sizeof(tss);
    tss.ist1 = (uint64_t)(uintptr_t)(ist1_stack + sizeof(ist1_stack)) & ~0xFULL;
    gdt_set_tss(6, (uint64_t)(uintptr_t)&tss, sizeof(tss) - 1); // TSS (0x30, uses slots 6+7)

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint64_t)(uintptr_t)&gdt;

    gdt_flush((uint64_t)(uintptr_t)&gdtp);
    tss_flush(GDT_TSS);
}

// One raw GDT descriptor, for the panic path: a #GP that names a selector is
// either about the selector's VALUE or about the descriptor it points at, and
// those two want completely different fixes.
uint64_t gdt_entry_raw(int index) {
    if (index < 0 || index > 7) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)gdt[index * 8 + i] << (i * 8);
    return v;
}

void tss_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
