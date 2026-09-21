#include "devices.h"
#include "kio.h"
#include "../drivers/pci.h"
#include "../drivers/xhci.h"
#include "../drivers/blkdev.h"
#include "../drivers/audio.h"
#include "../drivers/mouse.h"
#include "../drivers/framebuffer.h"
#include "../mm/pmm.h"
#include "../net/nic.h"
#include "../arch/x86_64/smp.h"
#include <stdint.h>

// The device list, assembled from the drivers that are already here.
//
// Nothing in this file probes anything: every fact is one a driver already
// established at boot. A device manager that went and poked hardware to draw a
// window would be a device manager that could hang the machine by being opened.

// --- tiny string helpers (the kernel has kprintf and nothing else) --------
static void scopy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void sapp(char *dst, const char *src, int cap) {
    int n = slen(dst);
    scopy(dst + n, src, cap - n);
}

static void uapp(char *dst, unsigned v, int cap) {
    char t[12];
    int n = 0;
    if (!v) { t[n++] = '0'; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char r[13];
    int j = 0;
    while (n) r[j++] = t[--n];
    r[j] = 0;
    sapp(dst, r, cap);
}

static void hexapp(char *dst, unsigned v, int digits, int cap) {
    static const char *H = "0123456789abcdef";
    char r[9];
    int j = 0;
    for (int i = digits - 1; i >= 0; i--) r[j++] = H[(v >> (i * 4)) & 0xF];
    r[j] = 0;
    sapp(dst, r, cap);
}

// --- CPU -----------------------------------------------------------------
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

// The brand string lives in three leaves of twelve bytes each. Machines that
// do not implement them get a name from the vendor leaf instead.
static void cpu_name(char *out, int cap) {
    uint32_t a, b, c, d;
    cpuid(0x80000000u, &a, &b, &c, &d);
    out[0] = 0;
    if (a >= 0x80000004u) {
        char buf[49];
        int p = 0;
        for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            cpuid(leaf, &a, &b, &c, &d);
            uint32_t regs[4] = { a, b, c, d };
            for (int r = 0; r < 4; r++)
                for (int i = 0; i < 4; i++)
                    buf[p++] = (char)((regs[r] >> (i * 8)) & 0xFF);
        }
        buf[p] = 0;
        int s = 0;
        while (buf[s] == ' ') s++;              // the brand string is padded
        scopy(out, buf + s, cap);
        if (out[0]) return;
    }
    cpuid(0, &a, &b, &c, &d);
    char v[13];
    uint32_t regs[3] = { b, d, c };             // vendor order is ebx, edx, ecx
    int p = 0;
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 4; i++)
            v[p++] = (char)((regs[r] >> (i * 8)) & 0xFF);
    v[p] = 0;
    scopy(out, v, cap);
}

// --- PCI naming ----------------------------------------------------------
static const char *vendor_name(uint16_t v) {
    switch (v) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x1002: return "AMD/ATI";
        case 0x10DE: return "NVIDIA";
        case 0x10EC: return "Realtek";
        case 0x1AF4: return "Red Hat (virtio)";
        case 0x1B36: return "Red Hat";
        case 0x1234: return "QEMU";
        case 0x15AD: return "VMware";
        case 0x1414: return "Microsoft";
        case 0x14E4: return "Broadcom";
        case 0x1969: return "Atheros";
        case 0x168C: return "Qualcomm Atheros";
        default: return 0;
    }
}

// PCI class codes are a published table; only the ones a desktop actually
// meets are worth spelling out.
static const char *class_name(uint8_t c, uint8_t sc, uint8_t pi) {
    switch (c) {
        case 0x01:
            switch (sc) {
                case 0x01: return "IDE storage controller";
                case 0x06: return "SATA controller";
                case 0x08: return "NVMe controller";
                default:   return "Storage controller";
            }
        case 0x02: return "Ethernet controller";
        case 0x03: return "Display controller";
        case 0x04:
            return (sc == 0x03) ? "HD Audio controller" : "Multimedia controller";
        case 0x06:
            switch (sc) {
                case 0x00: return "Host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI-to-PCI bridge";
                default:   return "Bridge";
            }
        case 0x0C:
            if (sc == 0x03) {
                switch (pi) {
                    case 0x00: return "USB UHCI controller";
                    case 0x10: return "USB OHCI controller";
                    case 0x20: return "USB EHCI controller";
                    case 0x30: return "USB xHCI controller";
                    default:   return "USB controller";
                }
            }
            return "Serial bus controller";
        case 0x00: return "Unclassified device";
        case 0x05: return "Memory controller";
        case 0x07: return "Communication controller";
        case 0x08: return "System peripheral";
        case 0x09: return "Input controller";
        case 0x0D: return "Wireless controller";
        default:   return "PCI device";
    }
}

static uint8_t class_category(uint8_t c) {
    switch (c) {
        case 0x01: return DEVC_STORAGE;
        case 0x02: return DEVC_NETWORK;
        case 0x03: return DEVC_DISPLAY;
        case 0x04: return DEVC_AUDIO;
        case 0x06: return DEVC_BRIDGE;
        case 0x09: return DEVC_INPUT;
        case 0x0C: return DEVC_USB;
        default:   return DEVC_OTHER;
    }
}

// Which of our drivers, if any, has claimed this device.
static const char *pci_driver_for(const pci_device_t *d, const char **status) {
    *status = "no driver";
    if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) {
        *status = xhci_present() ? "running" : "started, no devices";
        return "xhci";
    }
    if (d->class_code == 0x01 && d->vendor_id == 0x1AF4) {
        *status = (blkdev_backend() == 1) ? "running" : "idle";
        return "virtio-blk";
    }
    if (d->class_code == 0x02) {
        if (d->vendor_id == 0x10EC) {
            *status = nic_mac() ? (nic_link() ? "link up" : "link down") : "not initialised";
            return "rtl8125";
        }
        if (d->vendor_id == 0x8086) {
            *status = nic_mac() ? (nic_link() ? "link up" : "link down") : "not initialised";
            return "e1000";
        }
    }
    if (d->class_code == 0x04 && d->subclass == 0x03) {
        *status = audio_ready() ? "running" : "not initialised";
        return "hda";
    }
    if (d->class_code == 0x03) {
        *status = "in use (firmware)";
        return "vesa";
    }
    if (d->class_code == 0x06) { *status = "ok"; return ""; }
    return "";
}

// --- the list ------------------------------------------------------------
static struct si_dev *slot(struct si_dev *out, int max, int *n) {
    if (*n >= max) return 0;
    struct si_dev *d = &out[*n];
    for (unsigned i = 0; i < sizeof *d; i++) ((char *)d)[i] = 0;
    d->bus = d->slot = d->func = 0xFF;      // 0xFF means "not on the PCI bus"
    (*n)++;
    return d;
}

int device_list(struct si_dev *out, int max) {
    int n = 0;
    struct si_dev *d;

    // --- processor ---
    if ((d = slot(out, max, &n))) {
        d->cat = DEVC_CPU;
        cpu_name(d->name, sizeof d->name);
        if (!d->name[0]) scopy(d->name, "x86-64 processor", sizeof d->name);
        scopy(d->driver, "smp", sizeof d->driver);
        int nc = smp_cpu_count();
        d->status[0] = 0;
        uapp(d->status, (unsigned)(nc < 1 ? 1 : nc), sizeof d->status);
        sapp(d->status, nc == 1 ? " core" : " cores", sizeof d->status);
    }

    // --- memory ---
    if ((d = slot(out, max, &n))) {
        d->cat = DEVC_MEMORY;
        uint64_t total = pmm_total_frame_count() * (uint64_t)PAGE_SIZE;
        uint64_t freef = pmm_free_frame_count() * (uint64_t)PAGE_SIZE;
        scopy(d->name, "System memory ", sizeof d->name);
        uapp(d->name, (unsigned)(total >> 20), sizeof d->name);
        sapp(d->name, " MB", sizeof d->name);
        scopy(d->driver, "pmm", sizeof d->driver);
        uapp(d->status, (unsigned)(freef >> 20), sizeof d->status);
        sapp(d->status, " MB free", sizeof d->status);
    }

    // --- the screen we are drawing on ---
    if ((d = slot(out, max, &n))) {
        d->cat = DEVC_DISPLAY;
        scopy(d->name, "Framebuffer ", sizeof d->name);
        uapp(d->name, fb_get_width(), sizeof d->name);
        sapp(d->name, "x", sizeof d->name);
        uapp(d->name, fb_get_height(), sizeof d->name);
        sapp(d->name, " 32-bit", sizeof d->name);
        scopy(d->driver, "framebuffer", sizeof d->driver);
        scopy(d->status, "running", sizeof d->status);
    }

    // --- where the filesystem lives ---
    if ((d = slot(out, max, &n))) {
        d->cat = DEVC_STORAGE;
        int be = blkdev_backend();
        scopy(d->name, be == 1 ? "virtio-blk disk" :
                       be == 2 ? "RAM disk (boot module)" : "No block device",
              sizeof d->name);
        scopy(d->driver, be == 1 ? "virtio-blk" : be == 2 ? "ramdisk" : "",
              sizeof d->driver);
        scopy(d->status, be ? "mounted" : "absent", sizeof d->status);
    }

    // --- input ---
    if ((d = slot(out, max, &n))) {
        d->cat = DEVC_INPUT;
        int nk = xhci_keyboard_count();
        if (nk > 0) {
            scopy(d->name, "USB keyboard", sizeof d->name);
            scopy(d->driver, "xhci-hid", sizeof d->driver);
            if (nk > 1) { d->status[0] = 0; uapp(d->status, (unsigned)nk, sizeof d->status);
                          sapp(d->status, " attached", sizeof d->status); }
            else scopy(d->status, "running", sizeof d->status);
        } else {
            scopy(d->name, "PS/2 keyboard", sizeof d->name);
            scopy(d->driver, "i8042", sizeof d->driver);
            scopy(d->status, "running", sizeof d->status);
        }
    }
    if ((d = slot(out, max, &n))) {
        d->cat = DEVC_INPUT;
        int usb = xhci_mouse_present();
        scopy(d->name, usb ? "USB mouse" : "PS/2 mouse", sizeof d->name);
        scopy(d->driver, usb ? "xhci-hid" : "i8042-aux", sizeof d->driver);
        scopy(d->status, mouse_present() ? "running" : "not detected", sizeof d->status);
    }

    // --- audio, when there is no PCI controller to speak for it ---
    if (!audio_ready() && (d = slot(out, max, &n))) {
        d->cat = DEVC_AUDIO;
        scopy(d->name, "PC speaker", sizeof d->name);
        scopy(d->driver, "pcspk", sizeof d->driver);
        scopy(d->status, "fallback", sizeof d->status);
    }

    // --- everything on the PCI bus ---
    pci_device_t p;
    for (int i = 0; n < max && pci_device_n(i, &p); i++) {
        if ((d = slot(out, max, &n)) == 0) break;
        d->cat = class_category(p.class_code);
        d->bus = p.bus; d->slot = p.slot; d->func = p.func;
        d->vendor = p.vendor_id; d->device = p.device_id;

        const char *v = vendor_name(p.vendor_id);
        d->name[0] = 0;
        if (v) { sapp(d->name, v, sizeof d->name); sapp(d->name, " ", sizeof d->name); }
        sapp(d->name, class_name(p.class_code, p.subclass, p.prog_if), sizeof d->name);
        if (!v) {
            sapp(d->name, " [", sizeof d->name);
            hexapp(d->name, p.vendor_id, 4, sizeof d->name);
            sapp(d->name, ":", sizeof d->name);
            hexapp(d->name, p.device_id, 4, sizeof d->name);
            sapp(d->name, "]", sizeof d->name);
        }

        const char *st = 0;
        scopy(d->driver, pci_driver_for(&p, &st), sizeof d->driver);
        scopy(d->status, st ? st : "", sizeof d->status);
    }

    return n;
}
