#include "gdt.h"
#include "smp.h"
#include "../../mm/heap.h"
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

// Everything one core needs in order to run ring 3: a GDT, the task state
// segment the GDT points at, and the double-fault stack the TSS points at.
//
// ONE PER CORE, and each of the three for its own reason:
//
//   The TSS holds RSP0 -- where the processor puts the interrupt frame when
//   something interrupts ring 3. Two cores sharing it would drop their frames
//   onto the same stack, and the second would overwrite the first.
//
//   A TSS descriptor is marked busy when it is loaded, and loading a busy
//   one faults. So each core's TSS needs a descriptor of its own, which means
//   a GDT of its own.
//
//   IST1 is the stack a double fault runs on. Two cores faulting together on
//   one would each overwrite the other's frame in the middle of the panic.
struct cpu_desc {
    // 6 normal 8-byte descriptors (null, kcode, kdata, dummy32, udata, ucode)
    // followed by one 16-byte TSS descriptor -> 64 bytes total.
    uint8_t gdt[64];
    struct gdt_pointer gdtp;
    struct tss_struct tss;
};

#define IST1_SIZE 16384

static struct cpu_desc bsp_desc;
// A dedicated stack for the double-fault handler (IST1). If a process
// overflows its 16 KiB kernel stack, the CPU can no longer push the exception
// frame onto that stack -- without an IST that turns into a triple fault and
// a silent reset. Running #DF on this fresh stack means a clean panic instead.
static uint8_t bsp_ist1[IST1_SIZE] __attribute__((aligned(16)));

extern void gdt_flush(uint64_t gdt_pointer_addr);
extern void tss_flush(uint16_t selector);

static void gdt_set_entry(uint8_t *gdt, int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
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

static void gdt_set_tss(uint8_t *gdt, int index, uint64_t base, uint32_t limit) {
    gdt_set_entry(gdt, index, (uint32_t)base, limit, 0x89, 0x00);
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

static void build(struct cpu_desc *d, uint64_t ist1_top) {
    uint8_t *q = (uint8_t *)d;
    for (size_t i = 0; i < sizeof *d; i++) q[i] = 0;

    gdt_set_entry(d->gdt, 0, 0, 0, 0, 0);                    // null
    gdt_set_entry(d->gdt, 1, 0, 0xFFFFF, 0x9A, 0xA0);         // kernel code (0x08)
    gdt_set_entry(d->gdt, 2, 0, 0xFFFFF, 0x92, 0xC0);         // kernel data (0x10)
    gdt_set_entry(d->gdt, 3, 0, 0xFFFFF, 0x9A, 0xA0);         // dummy 32-bit user code (0x18, unused)
    gdt_set_entry(d->gdt, 4, 0, 0xFFFFF, 0xF2, 0xC0);         // user data (0x20|3)
    gdt_set_entry(d->gdt, 5, 0, 0xFFFFF, 0xFA, 0xA0);         // user code (0x28|3)

    d->tss.iomap_base = sizeof(d->tss);
    d->tss.ist1 = ist1_top & ~0xFULL;
    gdt_set_tss(d->gdt, 6, (uint64_t)(uintptr_t)&d->tss, sizeof(d->tss) - 1); // TSS (0x30, slots 6+7)

    d->gdtp.limit = sizeof(d->gdt) - 1;
    d->gdtp.base = (uint64_t)(uintptr_t)&d->gdt;
}

static void load(struct cpu_desc *d) {
    gdt_flush((uint64_t)(uintptr_t)&d->gdtp);
    tss_flush(GDT_TSS);
    this_cpu()->desc = d;
}

void gdt_init(void) {
    build(&bsp_desc, (uint64_t)(uintptr_t)(bsp_ist1 + IST1_SIZE));
    load(&bsp_desc);
}

void *gdt_prepare_cpu(void) {
    struct cpu_desc *d = (struct cpu_desc *)kmalloc(sizeof *d);
    uint8_t *ist1 = (uint8_t *)kmalloc(IST1_SIZE);
    if (!d || !ist1) {
        if (d) kfree(d);
        if (ist1) kfree(ist1);
        return 0;
    }
    build(d, (uint64_t)(uintptr_t)(ist1 + IST1_SIZE));
    return d;
}

void gdt_load_cpu(void *desc) {
    load((struct cpu_desc *)desc);
}

// One raw GDT descriptor, for the panic path: a #GP that names a selector is
// either about the selector's VALUE or about the descriptor it points at, and
// those two want completely different fixes.
uint64_t gdt_entry_raw(int index) {
    if (index < 0 || index > 7) return 0;
    struct cpu_desc *d = (struct cpu_desc *)this_cpu()->desc;
    if (!d) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)d->gdt[index * 8 + i] << (i * 8);
    return v;
}

void tss_set_kernel_stack(uint64_t rsp0) {
    struct cpu_desc *d = (struct cpu_desc *)this_cpu()->desc;
    if (d) d->tss.rsp0 = rsp0;
}
