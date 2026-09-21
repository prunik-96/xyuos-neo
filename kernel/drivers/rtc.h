#ifndef RTC_H
#define RTC_H

#include <stdint.h>

// Broken-down wall-clock time read from the CMOS RTC. Fields are the usual
// ranges; year is the full 4-digit year. The RTC keeps whatever the firmware
// set it to -- on most machines that is UTC.
struct rtc_time {
    int sec, min, hour;      // 0-59, 0-59, 0-23
    int day, mon, year;      // 1-31, 1-12, full year
};

// Read the current time from the CMOS RTC. Handles BCD/binary and 12/24-hour
// encodings and retries across an update-in-progress so the value is coherent.
void rtc_read(struct rtc_time *out);

// Seconds since the Unix epoch (1970-01-01 UTC) for a broken-down time, treating
// it as UTC. Cheap civil-from-days algorithm; good for file mtimes and stamps.
int64_t rtc_to_unix(const struct rtc_time *t);

// Convenience: current time as a Unix timestamp.
int64_t rtc_now_unix(void);

#endif
