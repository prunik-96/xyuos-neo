#include "elf.h"
#include "kio.h"
#include "../mm/vmm.h"
#include "../mm/paging.h"
#include "../mm/pmm.h"
#include <stddef.h>

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

#define PT_LOAD 1
#define ET_EXEC 2
#define EM_X86_64 0x3E

// Copy into a foreign address space one page at a time, translating each
// virtual page to its physical frame and writing through the kernel's identity
// map. Slower than a straight memcpy, but it works regardless of which address
// space is loaded in CR3.
static int copy_into_space(uint64_t pml4_phys, uint64_t vaddr,
                           const uint8_t *src, uint64_t n) {
    while (n > 0) {
        uint64_t off = vaddr & 0xFFF;
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;

        uint64_t phys = paging_translate(pml4_phys, vaddr);
        if (!phys) return -1;

        uint8_t *dst = (uint8_t *)(uintptr_t)phys;
        for (uint64_t i = 0; i < chunk; i++) dst[i] = src[i];

        vaddr += chunk;
        src += chunk;
        n -= chunk;
    }
    return 0;
}

int elf_load_into(uint64_t pml4_phys, const void *data, uint64_t size,
                  uint64_t *entry_out) {
    if (size < sizeof(elf64_ehdr_t)) {
        kprintf("elf: image too small\n");
        return -1;
    }

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != 2 /* ELFCLASS64 */) {
        kprintf("elf: bad magic/class\n");
        return -1;
    }
    if (eh->e_machine != EM_X86_64 || eh->e_type != ET_EXEC) {
        kprintf("elf: not a static x86_64 executable\n");
        return -1;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        kprintf("elf: program headers outside the image\n");
        return -1;
    }

    const uint8_t *base = (const uint8_t *)data;
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (base + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        if (ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > size) {
            kprintf("elf: segment %d outside the image\n", i);
            return -1;
        }
        if (!vmm_user_range_ok(ph->p_vaddr, ph->p_memsz)) {
            kprintf("elf: segment %d outside the user window\n", i);
            return -1;
        }

        // Freshly allocated frames arrive zeroed, which is exactly what .bss
        // (the p_filesz..p_memsz tail) needs -- no explicit clearing here.
        if (paging_map_alloc(pml4_phys, ph->p_vaddr, ph->p_memsz,
                             PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            kprintf("elf: out of memory mapping segment %d\n", i);
            return -1;
        }
        if (copy_into_space(pml4_phys, ph->p_vaddr,
                            base + ph->p_offset, ph->p_filesz) != 0) {
            kprintf("elf: failed to copy segment %d\n", i);
            return -1;
        }
    }

    if (!vmm_user_range_ok(eh->e_entry, 1)) {
        kprintf("elf: entry point outside the user window\n");
        return -1;
    }

    *entry_out = eh->e_entry;
    return 0;
}
