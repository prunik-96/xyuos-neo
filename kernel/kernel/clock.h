#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>
#include "../drivers/rtc.h"

// The wall clock: UTC, kept by the kernel, and the time zone it is shown in.
//
// The board's clock (the CMOS RTC) is read ONCE, at boot, and never written.
// What it holds is not known for certain: Linux keeps UTC there, Windows the
// local time. Until the network has said otherwise it is taken to be local
// time, as Windows keeps it, so a machine that also runs Windows shows the
// right time from the first second. The first NTP answer tells what the RTC
// really holds; that difference is remembered in /home/.clock, so the next
// boot is right before the network is up, and the RTC itself is left alone --
// another system on the machine keeps its clock exactly as it set it.
//
// From then on the time is the boot reading (or the last NTP answer) plus the
// microseconds the timestamp counter has counted since.

void    clock_init(void);                   // at boot, once the PIT is calibrated
void    clock_load(void);                   // once the disk is there: /home/.clock

int64_t clock_utc(void);                    // seconds since 1970, UTC
int64_t clock_utc_ms(void);
int     clock_offset(int64_t utc);          // seconds east of UTC at that moment, summer time included
int     clock_is_dst(int64_t utc);
void    clock_tm(int64_t t, struct rtc_time *out);   // broken down (no zone applied)
void    clock_local_tm(struct rtc_time *out);        // now, local

// Ask an NTP server (0: the usual pool) and set the clock from its answer.
// 1 on success; 0 no answer; -1 no network. `quick`: one server, few tries
// (the automatic sync when the network comes up must not hold it long).
int     clock_sync(const char *server, int quick);

// The zone, by its tz name ("Europe/Moscow"). 0, or -1 for an unknown name.
int         clock_set_zone(const char *name);
const char *clock_zone_name(void);
int         clock_zone_count(void);
// Zone i: its name, Russian and English labels, standard offset (minutes)
// and its summer-time rule (0 none, 1 European, 2 North American).
int         clock_zone_get(int i, const char **name, const char **ru, const char **en,
                           int *std_min, int *rule);
int         clock_zone_offset_now(int i);   // zone i's offset now, seconds

// What the last sync said.
struct clock_state {
    int     synced;          // set from NTP since boot
    int     rtc_known;       // what the board's clock holds is known
    int     rtc_skew;        // board clock minus UTC, seconds
    int64_t sync_utc;        // when, UTC seconds; 0 never (this boot or a past one)
    int     delta_ms;        // how far off the clock was then
    int     rtt_ms;          // how long the answer took
    char    server[64];
};
void clock_state(struct clock_state *out);

#endif
