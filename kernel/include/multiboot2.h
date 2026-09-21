#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

#define MULTIBOOT2_TAG_TYPE_END            0
#define MULTIBOOT2_TAG_TYPE_MODULE         3
#define MULTIBOOT2_TAG_TYPE_MMAP           6
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER     8
#define MULTIBOOT2_TAG_TYPE_ACPI_OLD       14  // RSDPv1 (32-bit RSDT)
#define MULTIBOOT2_TAG_TYPE_ACPI_NEW       15  // RSDPv2 (64-bit XSDT)

#define MULTIBOOT2_MEMORY_AVAILABLE         1

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
};

struct multiboot_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     cmdline[];
};

struct multiboot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
};

struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry entries[];
};

struct multiboot_info {
    uint32_t total_size;
    uint32_t reserved;
};

#endif
