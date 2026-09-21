#include "pit.h"
#include "../../drivers/audio.h"
#include "idt.h"
#include "smp.h"
#include "../../kernel/process.h"
#include "../../drivers/xhci.h"
#include "pic.h"
#include "../../include/port_io.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQ 1193182

static volatile uint64_t ticks = 0;
static volatile uint64_t idle_ticks = 0;
extern volatile int g_cpu_idle;   // set by the scheduler's idle loop (process.c)

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t tsc_per_us = 0;

// Measure the timestamp counter against PIT channel 2, which is the channel
// wired to the speaker: setting the gate without the speaker enable turns it
// into a stopwatch whose expiry can be read from port 0x61 bit 5. No interrupt
// is involved, so this works this early and would work just as well later.
static void calibrate_tsc(void) {
    const uint16_t count = 11932;               // ~10 ms at 1193182 Hz
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01)); // gate on, speaker off
    outb(PIT_COMMAND, 0xB0);                     // channel 2, lo/hi, mode 0
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));

    uint64_t c0 = rdtsc();
    // A machine that does not implement channel 2 would never raise OUT2, so
    // give up after far longer than the wait can legitimately take.
    while (!(inb(0x61) & 0x20)) {
        if (rdtsc() - c0 > 40000000000ULL) { outb(0x61, p61); return; }
    }
    uint64_t elapsed = rdtsc() - c0;
    outb(0x61, p61);

    tsc_per_us = elapsed / 10000;                // 10 ms = 10000 us
    if (!tsc_per_us) tsc_per_us = 1;
}

uint64_t pit_now_us(void) {
    // The fallback is the tick counter's own resolution -- coarse, and frozen
    // inside a system call, but better than a clock that reads zero.
    if (!tsc_per_us) return ticks * 10000;
    return rdtsc() / tsc_per_us;
}

static void pit_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    ticks++;
    if (g_cpu_idle) idle_ticks++;   // sample CPU utilisation
    smp_sample_load();              // per-core load for the System Monitor
    // Only flips sleeping processes back to READY; the actual switch happens
    // in sched_on_tick, after this handler returns.
    process_tick(ticks);
    // Poll the USB keyboard (no-op unless an xHCI keyboard was found). The PS/2
    // keyboard has its own IRQ; a USB one has to be polled, and the timer is the
    // always-running context to do it from.
    xhci_poll();
    // Advance a PC-speaker melody, if the machine has no HDA codec and one is
    // playing. A no-op otherwise.
    audio_tick();
}

void pit_init(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;

    outb(PIT_COMMAND, 0x36); // channel 0, lobyte/hibyte, mode 3 (square wave)
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    calibrate_tsc();

    irq_register_handler(0, pit_irq_handler);
    irq_unmask(0);
}

uint64_t pit_get_ticks(void) {
    return ticks;
}

uint64_t pit_idle_ticks(void) {
    return idle_ticks;
}
