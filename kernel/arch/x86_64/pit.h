#ifndef PIT_H
#define PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency_hz);
uint64_t pit_get_ticks(void);

// Microseconds since boot, from the timestamp counter. Unlike pit_get_ticks()
// this keeps advancing with interrupts disabled, which is the only reason a
// timeout inside a system call can ever expire. Calibrated by pit_init().
uint64_t pit_now_us(void);
// Ticks spent halted in the idle loop (for CPU-utilisation estimates).
uint64_t pit_idle_ticks(void);

#endif
