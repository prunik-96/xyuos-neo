// Reboot / power-off via ACPI. On UEFI the ACPI RSDP is NOT in the legacy BIOS
// area -- the firmware hands it to us through the multiboot2 ACPI tag, which is
// what we parse here. The DSDT \_S5 object gives the SLP_TYP value that, written
// to the FADT's PM1 control register with SLP_EN, powers the board off. Reset is
// the FADT reset register, with 8042 and triple-fault fallbacks.

#include "power.h"
#include "../include/multiboot2.h"
#include "../include/port_io.h"
#include "../kernel/kio.h"
#include "../mm/paging.h"
#include <stdint.h>
#include <stddef.h>

// Everything ACPI hands us is a physical address; the kernel identity-maps low
// memory, and paging_map_mmio can reach a high table if the firmware placed one
// there. For the low tables (typical) phys == virt.
static void *phys(uint64_t p) { return (void *)(uintptr_t)p; }

static uint32_t pm1a_cnt, pm1b_cnt;   // PM1 control register I/O ports
static uint16_t slp_typa, slp_typb;   // S5 sleep-type values (already << 10)
static int      s5_ok;
static uint32_t reset_port; static uint8_t reset_val; static int reset_ok;

#define SLP_EN (1u << 13)

struct sdt_header {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oemid[6], oemtableid[8];
    uint32_t oemrev;
    uint32_t creatorid, creatorrev;
} __attribute__((packed));

static int sig_is(const struct sdt_header *h, const char *s) {
    return h->sig[0] == s[0] && h->sig[1] == s[1] && h->sig[2] == s[2] && h->sig[3] == s[3];
}

// Find "_S5_" in the DSDT AML and pull SLP_TYPa/b out of its package. This is the
// well-worn minimal parser: it does not interpret AML, it pattern-matches the
// one object we need.
static void parse_s5(const struct sdt_header *dsdt) {
    const uint8_t *p = (const uint8_t *)dsdt + sizeof(*dsdt);
    const uint8_t *end = (const uint8_t *)dsdt + dsdt->length;
    for (; p + 4 < end; p++) {
        if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_') {
            // must be a NameOp (0x08), optionally preceded by '\'
            if (!((p[-1] == 0x08) || (p[-2] == 0x08 && p[-1] == '\\'))) continue;
            const uint8_t *q = p + 4;
            if (*q != 0x12) return;          // not a PackageOp
            q += 2;                          // skip PackageOp + PkgLength lead byte
            q += 1;                          // skip NumElements
            if (*q == 0x0A) q++;             // BytePrefix
            slp_typa = (uint16_t)(*q << 10);
            q++;
            if (*q == 0x0A) q++;
            slp_typb = (uint16_t)(*q << 10);
            s5_ok = 1;
            return;
        }
    }
}

static void parse_fadt(const struct sdt_header *fadt) {
    const uint8_t *b = (const uint8_t *)fadt;
    pm1a_cnt = *(const uint32_t *)(b + 64);
    pm1b_cnt = *(const uint32_t *)(b + 68);
    // Reset register (ACPI 2.0+): flags bit 10 in offset 112, address at 116
    // (Generic Address: space at +0, 64-bit address at +4), value at 128.
    if (fadt->length >= 129) {
        uint8_t space = b[116];
        uint64_t addr = *(const uint32_t *)(b + 116 + 4);   // low 32 bits suffice for I/O
        uint8_t val = b[128];
        if (space == 1 /* system I/O */ && addr) {
            reset_port = (uint32_t)addr; reset_val = val; reset_ok = 1;
        }
    }
    // DSDT: 32-bit at offset 40, 64-bit X_DSDT at 140 (prefer the 64-bit one).
    uint64_t dsdt = *(const uint32_t *)(b + 40);
    if (fadt->length >= 148) {
        uint64_t x = *(const uint64_t *)(b + 140);
        if (x) dsdt = x;
    }
    if (dsdt) parse_s5((const struct sdt_header *)phys(dsdt));
}

static void walk_sdt(uint64_t sdt_phys, int entry_bytes) {
    const struct sdt_header *root = (const struct sdt_header *)phys(sdt_phys);
    int n = (root->length - sizeof(*root)) / entry_bytes;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    for (int i = 0; i < n; i++) {
        uint64_t e = (entry_bytes == 8) ? ((const uint64_t *)entries)[i]
                                        : ((const uint32_t *)entries)[i];
        const struct sdt_header *h = (const struct sdt_header *)phys(e);
        if (sig_is(h, "FACP")) { parse_fadt(h); return; }
    }
}

void power_init(uint32_t multiboot_addr) {
    struct multiboot_info { uint32_t total_size, reserved; } *info =
        (struct multiboot_info *)(uintptr_t)multiboot_addr;
    uint8_t *tag_ptr = (uint8_t *)info + 8;

    for (;;) {
        struct multiboot_tag *tag = (struct multiboot_tag *)tag_ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) break;
        if (tag->type == MULTIBOOT2_TAG_TYPE_ACPI_OLD ||
            tag->type == MULTIBOOT2_TAG_TYPE_ACPI_NEW) {
            // The RSDP struct follows the 8-byte tag header.
            const uint8_t *rsdp = (const uint8_t *)tag + 8;
            uint8_t revision = rsdp[15];
            if (tag->type == MULTIBOOT2_TAG_TYPE_ACPI_NEW && revision >= 2) {
                uint64_t xsdt = *(const uint64_t *)(rsdp + 24);
                walk_sdt(xsdt, 8);
            } else {
                uint32_t rsdt = *(const uint32_t *)(rsdp + 16);
                walk_sdt(rsdt, 4);
            }
            break;
        }
        tag_ptr += (tag->size + 7) & ~7u;
    }
    kprintf("acpi: pm1a=%x s5=%s reset=%s\n", pm1a_cnt,
            s5_ok ? "yes" : "no", reset_ok ? "yes" : "no");
}

void power_off(void) {
    // ACPI S5 first, if we found everything.
    if (s5_ok && pm1a_cnt) {
        outw((uint16_t)pm1a_cnt, slp_typa | SLP_EN);
        if (pm1b_cnt) outw((uint16_t)pm1b_cnt, slp_typb | SLP_EN);
    }
    // Emulator fallbacks (QEMU isa-debug/ACPI, Bochs, VirtualBox).
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    // If we get here, nothing worked; halt so the machine is at least idle.
    for (;;) __asm__ volatile ("cli; hlt");
}

void power_reboot(void) {
    if (reset_ok) { outb((uint16_t)reset_port, reset_val); io_wait(); }
    // 8042 keyboard-controller pulse: assert the CPU reset line.
    for (int i = 0; i < 100; i++) {
        if (!(inb(0x64) & 0x02)) break;   // wait for input buffer empty
        io_wait();
    }
    outb(0x64, 0xFE);
    io_wait();
    // Last resort: a triple fault via a null IDT.
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" :: "m"(idt));
    for (;;) __asm__ volatile ("cli; hlt");
}
