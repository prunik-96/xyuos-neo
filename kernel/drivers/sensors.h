#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>

// What the processor says about itself: how hot it is, and how fast each core
// really runs.
//
// The temperature is AMD's (family 17h and later, Ryzen): the control
// temperature Tctl and each core die's (CCD), read from the System Management
// Network through the root complex, the way Linux's k10temp does. Only when
// the CPU is AMD's AND the root complex at 00:00.0 is AMD's -- in a virtual
// machine that register belongs to some other bridge and must not be touched.
//
// The frequency is the effective one: APERF counts the cycles a core really
// ran, MPERF the cycles it would have at its base clock, both only while it is
// awake; their ratio times the base clock is what it ran at. Sampled by every
// core on its own tick (the counters are per core).

#define SENSOR_NONE (-1000000)

void sensors_init(void);
void sensors_tick(void);                     // every timer tick, on every core

int  sensors_temp_mc(void);                  // Tctl in millidegrees, or SENSOR_NONE
int  sensors_ccd_mc(int i);                  // die i, or SENSOR_NONE
int  sensors_nccd(void);
int  sensors_core_mhz(int core);             // effective, 0 when not known
int  sensors_base_mhz(void);

#endif
