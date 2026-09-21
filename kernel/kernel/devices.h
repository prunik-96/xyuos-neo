#ifndef DEVICES_H
#define DEVICES_H

#include "../arch/x86_64/syscall.h"

// Build the machine's device list: what is in it, which driver claimed it, and
// whether that driver is actually running. Fills at most `max` entries and
// returns how many it wrote.
int device_list(struct si_dev *out, int max);

#endif
