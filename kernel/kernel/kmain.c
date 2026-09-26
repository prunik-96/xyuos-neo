#include "kio.h"
#include "task.h"
#include "elf.h"
#include "process.h"
#include "../include/multiboot2.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../crypto/crypto.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/apic.h"
#include "../arch/x86_64/smp.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/pit.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/syscall.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../mm/paging.h"
#include "../fs/vfs.h"
#include "../drivers/keyboard.h"
#include "../drivers/ps2mouse.h"
#include "../drivers/audio.h"
#include "../drivers/xhci.h"
#include "../drivers/power.h"
#include "../fs/fat32.h"
#include "../wm/wm.h"
#include "../gfx/font.h"
#include <stdint.h>
#include <stddef.h>


extern uint64_t p4_table[];   // the boot PML4
extern uint64_t p3_table[];   // the boot PDPT: 1 GiB slots over the low 512 GiB
extern uint64_t p2_table[];   // the boot PDs: 2 MiB pages over the low 4 GiB

// A spare PDPT for mapping the framebuffer when the firmware placed it in a
// high PML4 slot (above the 512 GiB that map_high_memory covers). Static, so no
// allocator is needed -- this runs before pmm_init. It lives in low .bss, which
// boot identity-maps, so its physical address equals its virtual one.
static uint64_t fb_high_pdpt[512] __attribute__((aligned(4096)));


// Make the framebuffer Write-Combining. Its default type is wrong for a linear
// framebuffer: below 4 GiB boot maps it Write-Back (pixel writes sit in cache,
// the display shows stale data -> heavy flicker); above 4 GiB it is Uncached
// (coherent but every write stalls, so an 8 MiB blit is visibly slow). WC is
// fast AND coherent. We flip the PAT bit (and clear PCD) on whichever existing
// page(s) already cover the FB range -- 2 MiB pages under 4 GiB, 1 GiB pages
// above -- then flush caches so no stale Write-Back lines linger.
static void fb_map_wc(uint64_t phys, uint64_t size) {
    if (size < 0x1000) size = 0x1000;
    uint64_t end = phys + size;
    for (uint64_t a = phys & ~0x1FFFFFULL; a < end && a < 0x100000000ULL; a += 0x200000ULL)
        p2_table[a >> 21] = (p2_table[a >> 21] & ~0x10ULL) | 0x1000ULL;  // clear PCD, set PAT
    for (uint64_t a = phys & ~0x3FFFFFFFULL;
         a < end && a >= 0x100000000ULL && a < 0x8000000000ULL; a += 0x40000000ULL)
        p3_table[a >> 30] = (p3_table[a >> 30] & ~0x10ULL) | 0x1000ULL;
    __asm__ volatile ("wbinvd; mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    // Say out loud what the mapping ended up as. A framebuffer that is quietly
    // Uncached instead of Write-Combining is the difference between a screen
    // copy at gigabytes a second and one at eighty megabytes a second, and
    // nothing else about the system looks any different when it happens.
    {
        uint64_t e = (phys < 0x100000000ULL) ? p2_table[phys >> 21]
                                             : p3_table[phys >> 30];
        unsigned pat = (e & 0x1000ULL) ? 1u : 0u;
        unsigned pcd = (e & 0x10ULL)   ? 1u : 0u;
        unsigned pwt = (e & 0x08ULL)   ? 1u : 0u;
        unsigned idx = (pat << 2) | (pcd << 1) | pwt;
        const char *nm = (idx == 4) ? "WC" : (idx == 0) ? "WB" :
                         (idx == 3) ? "UC" : (idx == 1) ? "WT" : "other";
        kprintf("fb: mapping PAT=%u PCD=%u PWT=%u -> PA%u = %s\n",
                pat, pcd, pwt, idx, nm);
    }
}

// Map a device region (the GOP framebuffer BAR) wherever the firmware put it,
// including ABOVE 512 GiB -- on AM5/UEFI with Above-4G decoding the GPU BAR can
// land at, e.g., 768 GiB, the same place the xHCI BAR does. 1 GiB uncached
// pages (device memory, not write-back RAM). Must run before the first write to
// the framebuffer, or that write faults and the machine triple-faults+resets.
static void map_device_region(uint64_t phys, uint64_t size) {
    if (size < 0x1000) size = 0x1000;
    uint64_t pml4_i = (phys >> 39) & 0x1FF;
    if (pml4_i == 0) return;   // within 0..512 GiB: already mapped by boot + map_high_memory

    if (!(p4_table[pml4_i] & 0x1)) {
        for (int k = 0; k < 512; k++) fb_high_pdpt[k] = 0;
        p4_table[pml4_i] = ((uint64_t)(uintptr_t)fb_high_pdpt) | 0x3ULL; // P|RW
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(p4_table[pml4_i] & 0x000FFFFFFFFFF000ULL);

    uint64_t start = phys & ~0x3FFFFFFFULL;
    uint64_t end   = (phys + size + 0x3FFFFFFFULL) & ~0x3FFFFFFFULL;
    for (uint64_t a = start; a < end; a += 0x40000000ULL) {
        uint64_t di = (a >> 30) & 0x1FF;
        pdpt[di] = a | 0x1ULL | 0x2ULL | 0x80ULL | 0x1000ULL; // P|RW|PS|PAT (Write-Combining)
    }
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

static void find_framebuffer(uint32_t multiboot_addr) {
    struct multiboot_info *info = (struct multiboot_info *)(uintptr_t)multiboot_addr;
    uint8_t *tag_ptr = (uint8_t *)info + 8;

    for (;;) {
        struct multiboot_tag *tag = (struct multiboot_tag *)tag_ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }
        if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
            struct multiboot_tag_framebuffer *fb = (struct multiboot_tag_framebuffer *)tag;
            // Ensure the framebuffer is reachable BEFORE fb_init hands it to the
            // console -- it may sit above 512 GiB on real UEFI hardware.
            uint64_t fb_size = (uint64_t)fb->framebuffer_pitch * fb->framebuffer_height;
            cpu_set_pat();                                  // enable the WC memory type
            map_device_region(fb->framebuffer_addr, fb_size);  // high FB: mapped WC
            fb_map_wc(fb->framebuffer_addr, fb_size);          // low/mid FB: flip to WC
            fb_init(fb->framebuffer_addr, fb->framebuffer_pitch,
                    fb->framebuffer_width, fb->framebuffer_height,
                    fb->framebuffer_bpp);
            kprintf("fb: %dx%d bpp=%d pitch=%d addr=0x%x%x\n",
                    fb->framebuffer_width, fb->framebuffer_height,
                    fb->framebuffer_bpp, fb->framebuffer_pitch,
                    (uint32_t)(fb->framebuffer_addr >> 32),
                    (uint32_t)fb->framebuffer_addr);
        }
        tag_ptr += (tag->size + 7) & ~7u;
    }
}

static void task_a_entry(void) {
    for (int i = 0; i < 5; i++) {
        kprintf("[task A] iteration %d\n", i);
        for (volatile int d = 0; d < 2000000; d++) { }
        yield();
    }
    kprintf("[task A] done\n");
    task_exit();
}

static void task_b_entry(void) {
    for (int i = 0; i < 5; i++) {
        kprintf("[task B] iteration %d\n", i);
        for (volatile int d = 0; d < 2000000; d++) { }
        yield();
    }
    kprintf("[task B] done\n");
    task_exit();
}

// TEMPORARY (P2): proves address spaces are genuinely private and that the
// kernel survives a CR3 switch -- the latter only holds if the kernel PDPT
// entries really are shared with the new space.
extern uint64_t p2_table[];

static void paging_selftest(void) {
    int pass = 0, fail = 0;
#define CHECK(cond, label) do { \
        if (cond) { kprintf("  PASS %s\n", label); pass++; } \
        else      { kprintf("  FAIL %s\n", label); fail++; } \
    } while (0)

    kprintf("paging selftest:\n");
    uint64_t kernel_space = paging_current();
    uint64_t free_before = pmm_free_frame_count();

    uint64_t as1 = paging_new_address_space();
    uint64_t as2 = paging_new_address_space();
    CHECK(as1 != 0 && as2 != 0 && as1 != as2, "created two address spaces");

    uint64_t va = USER_LOAD_BASE;
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    CHECK(paging_map_alloc(as1, va, 4096, flags) == 0 &&
          paging_map_alloc(as2, va, 4096, flags) == 0, "mapped the same VA in both");

    // The heart of it: one virtual address, two different physical pages.
    uint64_t phys1 = paging_translate(as1, va);
    uint64_t phys2 = paging_translate(as2, va);
    CHECK(phys1 != 0 && phys2 != 0 && phys1 != phys2, "the same VA maps to different frames");

    *(volatile uint64_t *)(uintptr_t)phys1 = 0x1111111111111111ULL;
    *(volatile uint64_t *)(uintptr_t)phys2 = 0x2222222222222222ULL;
    CHECK(*(volatile uint64_t *)(uintptr_t)phys1 == 0x1111111111111111ULL,
          "writing one space does not disturb the other");

    CHECK(paging_translate(as1, va + 0x100000) == 0, "unmapped address translates to 0");
    CHECK(paging_translate(as1, 0x100000) == 0x100000, "kernel identity map is shared");
    CHECK((p2_table[0] & PAGE_USER) == 0, "kernel pages are not user-accessible");

    // the dangerous part
    paging_switch(as1);
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)va;
    uint64_t readback = *p;
    paging_switch(kernel_space);

    CHECK(readback == 0x1111111111111111ULL, "read back through the loaded space");
    CHECK(paging_current() == kernel_space, "returned to the kernel space");
    kprintf("  kernel still alive after the CR3 round trip\n");

    paging_free_address_space(as1);
    paging_free_address_space(as2);
    CHECK(pmm_free_frame_count() == free_before, "every frame was returned on free");

    kprintf("paging selftest: %d passed, %d failed\n", pass, fail);
#undef CHECK
}

// The boot page tables identity-map only the first 4 GiB. On real UEFI
// hardware with Above-4G decoding the GPU framebuffer BAR often sits ABOVE
// 4 GiB, and the very first kprintf writes to it -- unmapped, that faults and
// the machine triple-faults and resets (the "reboot + no signal" seen on a
// B650M). Map 4 GiB..512 GiB with 1 GiB pages so any high framebuffer/MMIO is
// reachable. Uncached (PCD): these are device ranges, not write-back RAM.
//
// Guarded on PDPE1GB (CPUID 80000001h EDX[26]); every real x86-64 desktop has
// it. A VM CPU model without it skips this, which is fine there -- the
// framebuffer is below 4 GiB in QEMU anyway.

static void map_high_memory(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000001u));
    if (!(edx & (1u << 26))) return;   // no 1 GiB pages

    // Entries 0..3 stay the low 2 MiB map; entry 1 is the per-process user
    // window (set per address space). Fill 4..511 with 1 GiB device pages.
    for (uint64_t i = 4; i < 512; i++) {
        p3_table[i] = (i << 30) | 0x1ULL | 0x2ULL | 0x80ULL | 0x10ULL; // P|RW|PS|PCD
    }
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

void kernel_main(uint32_t multiboot_addr) {
    // Before anything else, the bootstrap core's per-CPU block: interrupt
    // handlers and the kernel lock find the core they are on through it, and
    // the first interrupt can come sooner than one might think.
    smp_early_init();
    serial_init();
    map_high_memory();       // before the first framebuffer write
    find_framebuffer(multiboot_addr);
    // SSE/FPU on, so the kernel can run floating-point code (the TrueType font
    // rasterizer). Safe here: IRQ handlers never touch SSE, so xmm state is
    // never clobbered underneath the font renderer. Every other core does the
    // same for itself in ap_entry().
    cpu_enable_sse();
    fb_set_color(0x0000FF80, 0x00000000);

    kprintf("xyuOS Neo -- booting...\n");
    kprintf("framebuffer: %s\n", fb_available() ? "OK" : "not available (serial only)");

    idt_init();

    // The physical allocator must come up before the APIC, whose MMIO mapping
    // needs a page-table frame; and the APIC must come up before the timer and
    // keyboard, so their IRQ lines are unmasked on the IO-APIC rather than the
    // (about-to-be-disabled) PIC.
    pmm_init(multiboot_addr);
    kprintf("pmm: %u free frames of %u total\n",
            (unsigned int)pmm_free_frame_count(), (unsigned int)pmm_total_frame_count());

    if (!apic_init(multiboot_addr))
        kprintf("apic: unavailable, falling back to the 8259 PIC\n");

    pit_init(100);
    keyboard_init();
    // A PS/2 mouse, if the machine has one. Real hardware usually puts the
    // pointer on USB (handled later, in xhci_init), but a virtual machine
    // started without an explicit USB mouse offers only this one -- and with
    // no driver for it the desktop has a cursor nobody can move.
    ps2mouse_init();
    __asm__ volatile ("sti");

    heap_init();
    gdt_init();
    syscall_init();
    paging_selftest();

    // Wake the other cores (needs the heap for AP stacks, the APIC for IPIs,
    // and interrupts on for the inter-SIPI delays). They idle until the SMP
    // scheduler (B4) gives them work.
    smp_init();

    if (font_init(22)) {
        kprintf("font: AA TrueType ready, cell %ux%u\n",
                (unsigned)font_cell_w(), (unsigned)font_cell_h());
    } else {
        kprintf("font: init FAILED\n");
    }

    // required before any ring3 code runs: the CPU uses TSS.RSP0 as the
    // kernel stack for interrupts/exceptions taken while in user mode.
    uint8_t *irq_stack = (uint8_t *)kmalloc(16384);
    tss_set_kernel_stack((uint64_t)(uintptr_t)(irq_stack + 16384));

    fb_set_color(0x00FFFFFF, 0x00000000);
    kprintf("xyuOS Neo booted. Interrupts enabled, timer running.\n");

    // A cipher that is subtly wrong fails later as something that looks
    // like a network fault, so check it here where the answer is
    // unambiguous. Costs a few milliseconds and prints one line.
    crypto_selftest();

    sched_init();
    task_create("A", task_a_entry);
    task_create("B", task_b_entry);

    for (int i = 0; i < 3; i++) {
        kprintf("[main] iteration %d\n", i);
        for (volatile int d = 0; d < 2000000; d++) { }
        yield();
    }
    kprintf("[main] scheduler demo done\n");

    // USB keyboard: needed on UEFI-only hardware where there is no PS/2 at port
    // 0x60. Harmless in QEMU/BIOS -- returns 0 if there is no xHCI, and the
    // PS/2 driver keeps working alongside it. xhci_poll() is driven by the
    // timer tick (see pit.c).
    power_init(multiboot_addr);   // ACPI: learn how to power off / reset

    // Before the filesystem: the root filesystem may be on the USB stick the
    // machine booted from, and xhci_init is what finds the sticks.
    if (xhci_init()) {
        kprintf("usb: keyboard ready\n");
    }
    kprintf("usbdisk: %d device(s)\n", usb_disk_count());

    if (vfs_init(multiboot_addr)) {
        kprintf("vfs: ext2 root filesystem mounted\n");
    } else {
        kprintf("vfs: mount failed (no disk and no RAM-disk module)\n");
    }

    // Sound. Failure here is not fatal: sound_alert() falls back to the PC
    // speaker, and a machine that can report its own errors out loud is worth
    // more than one that stays silent because the codec did not answer.
    audio_init();
    if (rndis_present()) {
        const uint8_t *m = rndis_mac();
        kprintf("rndis: USB NIC ready, mac %x:%x:%x:%x:%x:%x\n",
                m[0], m[1], m[2], m[3], m[4], m[5]);
    } else if (rndis_status()[0] != 'n') {   // a device was found but not ready
        kprintf("rndis: not ready (%s cc=%d)\n", rndis_status(), rndis_last_cc());
    }


    // Boot self-tests and the P2..P5 demos lived here; they are recorded in
    // the project notes and were removed once the shell became the real UI.
    // Everything they proved is now exercised by simply using the system.

    // The window manager is the compositor: it owns the tiling tree, the pane
    // grids, the framebuffer and the keyboard, and nothing else. The shell
    // that appears in the first pane is an ordinary userland process, and so
    // is every program it starts.
    //
    // wm_start() only sets the tree up and spawns /bin/sh; the compositor then
    // runs from the scheduler's idle path (wm_poll), which is exactly when
    // every process is blocked waiting for the input it is about to deliver.
    wm_start();
    process_run_all();

    kprintf("all processes exited; nothing left to run\n");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
