// CMOS real-time clock. Read register N by writing N to port 0x70 and reading
// port 0x71. Register 0x0A bit 7 is "update in progress" -- read only when it is
// clear, and read the whole time twice to be sure an update didn't slip in the
// middle. Status register 0x0B tells us the encoding: bit 2 = binary (else BCD),
// bit 1 = 24-hour (else 12-hour with bit 7 of the hour = PM).

#include "rtc.h"
#include "../include/port_io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

void rtc_read(struct rtc_time *out) {
    uint8_t sec, min, hour, day, mon, year, century = 0;
    uint8_t last_sec = 0xFF, last_min = 0, last_hour = 0, last_day = 0,
            last_mon = 0, last_year = 0, last_cent = 0;

    // Spin past any in-progress update, then read; repeat until two consecutive
    // reads agree (guards against a rollover between the first and last byte).
    for (int guard = 0; guard < 1000000; guard++) {
        // Bounded wait: a healthy RTC clears "update in progress" within ~1ms;
        // never spin forever if some firmware leaves the bit stuck.
        for (int w = 0; w < 100000 && update_in_progress(); w++) { }
        sec  = cmos_read(0x00);
        min  = cmos_read(0x02);
        hour = cmos_read(0x04);
        day  = cmos_read(0x07);
        mon  = cmos_read(0x08);
        year = cmos_read(0x09);
        century = cmos_read(0x32);
        if (sec == last_sec && min == last_min && hour == last_hour &&
            day == last_day && mon == last_mon && year == last_year &&
            century == last_cent)
            break;
        last_sec = sec; last_min = min; last_hour = hour; last_day = day;
        last_mon = mon; last_year = year; last_cent = century;
    }

    uint8_t regb = cmos_read(0x0B);
    int is_bin = regb & 0x04;
    int is_24h = regb & 0x02;
    int pm = (!is_24h) && (hour & 0x80);   // in 12h mode, bit 7 marks PM
    if (!is_24h) hour &= 0x7F;

    if (!is_bin) {
        sec = bcd_to_bin(sec); min = bcd_to_bin(min); hour = bcd_to_bin(hour);
        day = bcd_to_bin(day); mon = bcd_to_bin(mon); year = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    if (!is_24h) {
        if (pm && hour != 12) hour = (uint8_t)(hour + 12);
        else if (!pm && hour == 12) hour = 0;
    }

    int full_year;
    if (century >= 19 && century <= 21) full_year = century * 100 + year;
    else                                full_year = 2000 + year;   // no century reg

    out->sec = sec; out->min = min; out->hour = hour;
    out->day = day; out->mon = mon; out->year = full_year;
}

// Days from the civil date to 1970-01-01 (Howard Hinnant's algorithm).
static int64_t days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t rtc_to_unix(const struct rtc_time *t) {
    int64_t days = days_from_civil(t->year, t->mon, t->day);
    return ((days * 24 + t->hour) * 60 + t->min) * 60 + t->sec;
}

int64_t rtc_now_unix(void) {
    struct rtc_time t;
    rtc_read(&t);
    return rtc_to_unix(&t);
}
