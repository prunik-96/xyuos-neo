// The processor's own thermometer and clock. See sensors.h.

#include "sensors.h"
#include "pci.h"
#include "../arch/x86_64/smp.h"
#include "../kernel/kio.h"

uint64_t pit_tsc_mhz(void);

static int smn_ok;            // the SMN can be read: an AMD Ryzen on an AMD root complex
static int ccd_offset;        // where the dies' temperatures are, past the Tctl register
static int nccd_max;
static int eff_ok;            // APERF/MPERF are there
static int base_mhz;

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

// One SMN register, through the root complex's index/data pair (0x60/0x64).
// Under the kernel lock, so nobody else is between the two.
static uint32_t smn_read(uint32_t addr) {
    pci_config_write32(0, 0, 0, 0x60, addr);
    return pci_config_read32(0, 0, 0, 0x64);
}

#define TEMP_CTRL 0x00059800u

void sensors_init(void) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    int amd = b == 0x68747541 && d == 0x69746E65 && c == 0x444D4163;   // "AuthenticAMD"
    cpuid(1, &a, &b, &c, &d);
    int family = (int)((a >> 8) & 0xF), model = (int)((a >> 4) & 0xF);
    if (family == 0xF) { family += (int)((a >> 20) & 0xFF); model |= (int)((a >> 16) & 0xF) << 4; }

    base_mhz = (int)pit_tsc_mhz();

    // APERF/MPERF: CPUID 6, ECX bit 0, on both makers' processors.
    cpuid(0, &a, &b, &c, &d);
    if (a >= 6) {
        uint32_t a6, b6, c6, d6;
        cpuid(6, &a6, &b6, &c6, &d6);
        eff_ok = c6 & 1;
    }

    // The thermometer: an AMD Zen, and its root complex really AMD's.
    if (amd && family >= 0x17) {
        uint32_t id = pci_config_read32(0, 0, 0, 0);
        if ((id & 0xFFFF) == 0x1022) {
            smn_ok = 1;
            if (family == 0x17) {
                if (model == 0x31 || model == 0x71 || model == 0x60 || model == 0x68 || model == 0xA0) {
                    ccd_offset = 0x154; nccd_max = 8;
                }
            } else if (family == 0x19) {
                if (model <= 0x01 || model == 0x08 || model == 0x21 || (model >= 0x50 && model <= 0x5F)) {
                    ccd_offset = 0x154; nccd_max = 8;
                } else if (model >= 0x40 && model <= 0x4F) {
                    ccd_offset = 0x300; nccd_max = 8;
                } else if (model >= 0x60 && model <= 0x7F) {      // Raphael (Ryzen 7000), Phoenix
                    ccd_offset = 0x308; nccd_max = 8;
                } else if ((model >= 0x10 && model <= 0x1F) || (model >= 0xA0 && model <= 0xAF)) {
                    ccd_offset = 0x300; nccd_max = 12;
                }
            } else if (family == 0x1A) {
                ccd_offset = 0x308; nccd_max = 8;
            }
        }
    }
    kprintf("sensors: %s family %x model %x, temperature %s, frequency %s, base %d MHz\n",
            amd ? "AMD" : "other", family, model, smn_ok ? "yes" : "no", eff_ok ? "yes" : "no", base_mhz);
}

int sensors_temp_mc(void) {
    if (!smn_ok) return SENSOR_NONE;
    uint32_t v = smn_read(TEMP_CTRL);
    int t = (int)((v >> 21) * 125);
    // The register reads -49..206 C when its range bit is set (or both
    // "TJ select" bits are).
    if ((v & (1u << 19)) || ((v >> 16) & 3) == 3) t -= 49000;
    return t;
}

int sensors_nccd(void) {
    if (!smn_ok || !ccd_offset) return 0;
    int n = 0;
    for (int i = 0; i < nccd_max; i++) if (smn_read(TEMP_CTRL + (uint32_t)ccd_offset + 4u * (uint32_t)i) & (1u << 11)) n = i + 1;
    return n;
}

int sensors_ccd_mc(int i) {
    if (!smn_ok || !ccd_offset || i < 0 || i >= nccd_max) return SENSOR_NONE;
    uint32_t v = smn_read(TEMP_CTRL + (uint32_t)ccd_offset + 4u * (uint32_t)i);
    if (!(v & (1u << 11))) return SENSOR_NONE;
    return (int)((v & 0x7FF) * 125) - 49000;
}

// --- the effective clock, per core -------------------------------------------------

static uint64_t last_a[MAX_CPUS], last_m[MAX_CPUS];
static uint32_t ticks[MAX_CPUS];
static volatile int mhz[MAX_CPUS];

void sensors_tick(void) {
    if (!eff_ok) return;
    int c = (int)this_cpu()->cpu_index;
    if (c < 0 || c >= MAX_CPUS) return;
    if (++ticks[c] < 50) return;                 // half a second at 100 Hz
    ticks[c] = 0;
    uint64_t a = rdmsr(0xE8), m = rdmsr(0xE7);   // APERF, MPERF
    uint64_t da = a - last_a[c], dm = m - last_m[c];
    // A core that slept the whole time ran no cycles to measure: it keeps
    // what it last showed.
    if (last_m[c] && dm > 1000)
        mhz[c] = (int)((uint64_t)base_mhz * da / dm);
    last_a[c] = a;
    last_m[c] = m;
}

int sensors_core_mhz(int core) { return core >= 0 && core < MAX_CPUS ? mhz[core] : 0; }
int sensors_base_mhz(void) { return base_mhz; }
