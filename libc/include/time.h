#ifndef TIME_H
#define TIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec, tm_min, tm_hour;
    int tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

// UTC seconds since 1970, from the kernel's clock (set over NTP).
time_t     time(time_t *t);

/* Real conversions, in both directions: mktime(localtime(t)) and
 * timegm(gmtime(t)) give back t. Local time is the zone set in the control
 * panel (or `date zone`), summer time included. */
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);
time_t     mktime(struct tm *tm);
time_t     timegm(struct tm *tm);
/* difftime returns a double, so it is defined in math.c -- the one file in
 * this libc compiled with floating-point registers. Declared here because
 * this is where callers look for it. */
double     difftime(time_t a, time_t b);

char      *asctime(const struct tm *tm);
char      *ctime(const time_t *t);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

#ifdef __cplusplus
}
#endif

#endif
