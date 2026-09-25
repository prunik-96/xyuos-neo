#include "apic.h"
#include "pic.h"
#include "../../include/multiboot2.h"
#include "../../include/port_io.h"
#include "../../kernel/kio.h"
#include "../../mm/paging.h"
#include <stddef.h>

// ==========================================================================
//  ACPI discovery (self-contained: find the MADT via the multiboot2 RSDP)
// ==========================================================================

struct sdt_header {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oemid[6], oemtableid[8];
    uint32_t oemrev, creatorid, creatorrev;
} __attribute__((packed));

// ACPI tables live in low reclaimable RAM, which the kernel identity-maps.
static void *phys(uint64_t p) { return (void *)(uintptr_t)p; }

static int sig_is(const struct sdt_header *h, const char *s) {
    return h->sig[0] == s[0] && h->sig[1] == s[1] && h->sig[2] == s[2] && h->sig[3] == s[3];
}

static const struct sdt_header *find_table(uint64_t root_phys, int entry_bytes, const char *sig) {
    const struct sdt_header *root = phys(root_phys);
    int n = (root->length - sizeof(*root)) / entry_bytes;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    for (int i = 0; i < n; i++) {
        uint64_t e = (entry_bytes == 8) ? ((const uint64_t *)entries)[i]
                                        : ((const uint32_t *)entries)[i];
        const struct sdt_header *h = phys(e);
        if (sig_is(h, sig)) return h;
    }
    return NULL;
}

static const struct sdt_header *find_madt(uint32_t multiboot_addr) {
    struct { uint32_t total_size, reserved; } *info =
        (void *)(uintptr_t)multiboot_addr;
    uint8_t *tag_ptr = (uint8_t *)info + 8;
    for (;;) {
        struct multiboot_tag *tag = (struct multiboot_tag *)tag_ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) break;
        if (tag->type == MULTIBOOT2_TAG_TYPE_ACPI_OLD ||
            tag->type == MULTIBOOT2_TAG_TYPE_ACPI_NEW) {
            const uint8_t *rsdp = (const uint8_t *)tag + 8;
            uint8_t revision = rsdp[15];
            if (tag->type == MULTIBOOT2_TAG_TYPE_ACPI_NEW && revision >= 2)
                return find_table(*(const uint64_t *)(rsdp + 24), 8, "APIC");
            return find_table(*(const uint32_t *)(rsdp + 16), 4, "APIC");
        }
        tag_ptr += (tag->size + 7) & ~7u;
    }
    return NULL;
}

// ==========================================================================
//  Local APIC + IO-APIC
// ==========================================================================

#define IA32_APIC_BASE 0x1B

#define LAPIC_ID   0x020
#define LAPIC_EOI  0x0B0
#define LAPIC_SVR  0x0F0
#define LAPIC_TPR  0x080

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WIN    0x10

#define MAX_CPUS 32

static volatile uint8_t *lapic = NULL;

// The EOI register itself, for the two interrupt handlers written in assembly
// (isr.S: the wake-up and the TLB shootdown), which acknowledge the interrupt
// without going anywhere near C.
volatile uint32_t *lapic_eoi_reg;
static volatile uint8_t *ioapic = NULL;
static uint32_t ioapic_gsi_base = 0;
static uint32_t bsp_id = 0;
static int active = 0;

static uint8_t cpu_ids[MAX_CPUS];
static int     ncpu = 0;

// Interrupt source overrides: ISA IRQ -> GSI + polarity/trigger flags.
static struct { uint32_t gsi; uint16_t flags; int used; } ovr[16];

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static void lapic_write(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(lapic + reg) = v; }
static uint32_t lapic_read(uint32_t reg) { return *(volatile uint32_t *)(lapic + reg); }

static void ioapic_write(uint32_t reg, uint32_t v) {
    *(volatile uint32_t *)(ioapic + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t *)(ioapic + IOAPIC_WIN) = v;
}

// Program one IO-APIC redirection entry for `gsi` -> `vector` on the BSP.
static void ioapic_route(uint32_t gsi, uint8_t vector, uint16_t flags, int masked) {
    uint32_t low = vector;                         // fixed, physical, edge, high
    if ((flags & 0x3) == 0x3)        low |= (1u << 13);   // active low
    if (((flags >> 2) & 0x3) == 0x3) low |= (1u << 15);   // level triggered
    if (masked)                      low |= (1u << 16);
    uint32_t idx = 0x10 + (gsi - ioapic_gsi_base) * 2;
    ioapic_write(idx + 1, bsp_id << 24);           // destination APIC id
    ioapic_write(idx, low);
}

static void resolve(uint8_t irq, uint32_t *gsi, uint16_t *flags) {
    if (irq < 16 && ovr[irq].used) { *gsi = ovr[irq].gsi; *flags = ovr[irq].flags; }
    else { *gsi = irq; *flags = 0; }
}

int apic_init(uint32_t multiboot_addr) {
    const struct sdt_header *madt = find_madt(multiboot_addr);
    if (!madt) { kprintf("apic: no MADT\n"); return 0; }

    const uint8_t *b = (const uint8_t *)madt;
    uint64_t lapic_phys = *(const uint32_t *)(b + 36);
    uint32_t madt_flags = *(const uint32_t *)(b + 40);

    const uint8_t *p = b + 44;                      // header(36)+lapicaddr(4)+flags(4)
    const uint8_t *end = b + madt->length;
    uint64_t ioapic_phys = 0;
    while (p < end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2) break;
        switch (type) {
            case 0:                                 // processor local APIC
                if ((*(const uint32_t *)(p + 4) & 1) && ncpu < MAX_CPUS)
                    cpu_ids[ncpu++] = p[3];
                break;
            case 1:                                 // IO APIC
                if (!ioapic_phys) {
                    ioapic_phys = *(const uint32_t *)(p + 4);
                    ioapic_gsi_base = *(const uint32_t *)(p + 8);
                }
                break;
            case 2: {                               // interrupt source override
                uint8_t src = p[3];
                if (src < 16) {
                    ovr[src].gsi = *(const uint32_t *)(p + 4);
                    ovr[src].flags = *(const uint16_t *)(p + 8);
                    ovr[src].used = 1;
                }
                break;
            }
            case 5:                                 // 64-bit LAPIC address override
                lapic_phys = *(const uint64_t *)(p + 4);
                break;
        }
        p += len;
    }
    if (!ioapic_phys) { kprintf("apic: no IO-APIC\n"); return 0; }

    // Map the APIC MMIO uncached (both sit in the top of the 4th GiB).
    paging_map_mmio(lapic_phys, 0x1000);
    paging_map_mmio(ioapic_phys, 0x1000);
    lapic = (volatile uint8_t *)(uintptr_t)lapic_phys;
    lapic_eoi_reg = (volatile uint32_t *)(lapic + LAPIC_EOI);
    ioapic = (volatile uint8_t *)(uintptr_t)ioapic_phys;

    // If the board booted in PIC mode (PCAT_COMPAT), flip the IMCR so the
    // interrupt lines go to the APIC instead of the 8259s.
    if (madt_flags & 1) { outb(0x22, 0x70); outb(0x23, 0x01); }
    pic_disable();                                  // mask every 8259 line

    active = 1;
    apic_enable_local();                             // enable the BSP's Local APIC
    bsp_id = lapic_read(LAPIC_ID) >> 24;

    kprintf("apic: LAPIC @%x IO-APIC @%x cpus=%d bsp=%d\n",
            (unsigned)lapic_phys, (unsigned)ioapic_phys, ncpu, (int)bsp_id);
    return 1;
}

int  apic_active(void) { return active; }
void lapic_eoi(void)   { if (lapic) lapic_write(LAPIC_EOI, 0); }

// Enable the Local APIC of the core that calls this. EACH core has its own
// LAPIC and must software-enable it (the enable bit is clear after reset), or
// it will not receive IPIs. Called on the BSP by apic_init and on every AP.
void apic_enable_local(void) {
    if (!lapic) return;
    wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) | (1u << 11));  // hardware enable
    lapic_write(LAPIC_TPR, 0);                                  // accept all priorities
    lapic_write(LAPIC_SVR, 0x100 | 0xFF);                       // software enable | spurious 0xFF
}

// IRQ2 is the 8259 cascade line. It is not a device interrupt, and on an
// IO-APIC its GSI usually belongs to something else entirely (firmware
// commonly overrides the timer onto GSI 2). Routing it would silently steal
// that entry, so refuse -- a driver asking for it wants the PIC behaviour and
// there is nothing to do here.
void apic_unmask_irq(uint8_t irq) {
    if (!active || irq == 2) return;
    uint32_t gsi; uint16_t flags; resolve(irq, &gsi, &flags);
    ioapic_route(gsi, (uint8_t)(32 + irq), flags, 0);
}
void apic_mask_irq(uint8_t irq) {
    if (!active || irq == 2) return;
    uint32_t gsi; uint16_t flags; resolve(irq, &gsi, &flags);
    ioapic_route(gsi, (uint8_t)(32 + irq), flags, 1);
}

int      apic_cpu_count(void) { return ncpu; }
const uint8_t *apic_cpu_ids(void) { return cpu_ids; }
uint32_t apic_bsp_id(void) { return bsp_id; }

// --- inter-processor interrupts (SMP bring-up) ----------------------------
#define LAPIC_ICR_LOW  0x300
#define LAPIC_ICR_HIGH 0x310

static void icr_wait(void) {
    while (lapic_read(LAPIC_ICR_LOW) & (1u << 12)) __asm__ volatile ("pause");
}

void lapic_send_init(uint8_t apic_id) {
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, 0x4500);          // INIT | assert | edge
    icr_wait();
}

void lapic_send_sipi(uint8_t apic_id, uint8_t vector) {
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, 0x4600 | vector); // Startup | vector
    icr_wait();
}

// Fixed IPI to every other core (dest shorthand "all excluding self"). Used to
// wake the application processors out of their idle hlt.
void lapic_broadcast_ipi(uint8_t vector) {
    if (!active) return;
    lapic_write(LAPIC_ICR_LOW, 0x000C0000 | vector);  // all-excl-self | fixed | vector
    icr_wait();
}
