/* The calendar: turning a count of seconds into a date and back.
 *
 * There is no clock chip driver in this system, so the date is fixed at a
 * plainly wrong one rather than a pretend-accurate one -- that decision is
 * in posix.c and stands. What was missing is everything downstream of it.
 *
 * It matters more than it sounds. A browser reads dates constantly: Expires,
 * Last-Modified, If-Modified-Since, a cookie's lifetime. It parses them into
 * a struct tm and calls mktime, then compares. A gmtime that ignored its
 * argument and handed back a constant -- which is what was here -- makes
 * every one of those comparisons come out the same way, and nothing looks
 * broken. It just quietly always answers yes.
 *
 * So the conversions here are real ones, in both directions, and they agree:
 * mktime(gmtime(t)) == t for every t. The clock being wrong is a separate
 * and honest limitation; arithmetic on it should still be arithmetic.
 *
 * The days-to-date conversion is Howard Hinnant's, which shifts the year to
 * start in March so that the leap day falls at the end and the month lengths
 * form a repeating pattern with no special cases.
 */

#include <time.h>
#include <stdio.h>

#define SECS_PER_DAY 86400L

/* Days since 1970-01-01 for a civil date. Valid for any year. */
static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);              /* [0, 399] */
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

static void civil_from_days(long z, long *y, unsigned *m, unsigned *d) {
    z += 719468L;
    long era = (z >= 0 ? z : z - 146096L) / 146097L;
    unsigned doe = (unsigned)(z - era * 146097L);          /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    long yy = (long)yoe + era * 400L;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10u ? 3u : (unsigned)-9);
    *y = yy + (*m <= 2u);
}

static int leap(long y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

struct tm *gmtime(const time_t *t) {
    static struct tm tm;
    if (t == NULL) return NULL;

    long secs = *t;
    long days = secs / SECS_PER_DAY;
    long rem  = secs % SECS_PER_DAY;
    if (rem < 0) { rem += SECS_PER_DAY; days--; }   /* floor, not truncate */

    tm.tm_hour = (int)(rem / 3600);
    tm.tm_min  = (int)((rem / 60) % 60);
    tm.tm_sec  = (int)(rem % 60);

    long y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);

    tm.tm_year = (int)(y - 1900);
    tm.tm_mon  = (int)m - 1;
    tm.tm_mday = (int)d;

    /* 1970-01-01 was a Thursday. */
    long wd = (days + 4) % 7;
    if (wd < 0) wd += 7;
    tm.tm_wday = (int)wd;

    tm.tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm.tm_isdst = 0;
    return &tm;
}

/* This system has no timezone database and no way to learn one, so local
 * time is UTC. Saying so here is better than an offset invented from
 * nothing. */
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm) {
    if (tm == NULL) return (time_t)-1;

    /* Fields out of range are normalised, as the standard requires: callers
     * do arithmetic like "tm_mday += 30" and expect the month to follow. */
    long year = (long)tm->tm_year + 1900;
    long mon = tm->tm_mon;
    year += mon / 12;
    mon %= 12;
    if (mon < 0) { mon += 12; year--; }

    long days = days_from_civil(year, (unsigned)mon + 1, 1) + (tm->tm_mday - 1);
    long secs = days * SECS_PER_DAY
              + (long)tm->tm_hour * 3600
              + (long)tm->tm_min * 60
              + (long)tm->tm_sec;

    /* Hand back the normalised form, which is what mktime is for. */
    time_t t = (time_t)secs;
    struct tm *n = gmtime(&t);
    if (n != NULL) *tm = *n;
    return t;
}

static const char wdays[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu",
                                  "Fri", "Sat" };
static const char months[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

char *asctime(const struct tm *tm) {
    static char buf[32];
    if (tm == NULL) return NULL;
    int wd = tm->tm_wday, mo = tm->tm_mon;
    if (wd < 0 || wd > 6) wd = 0;
    if (mo < 0 || mo > 11) mo = 0;
    snprintf(buf, sizeof buf, "%s %s %2d %02d:%02d:%02d %ld\n",
             wdays[wd], months[mo], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             (long)tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *t) { return asctime(gmtime(t)); }

/* A small subset: the conversions a log line or an HTTP date needs. An
 * unknown one is copied through with its percent, so a caller sees what it
 * asked for rather than nothing. */
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;

#define PUT(c) do { if (n + 1 >= max) { if (max) s[n] = 0; return 0; } \
                    s[n++] = (char)(c); } while (0)
#define PUTS(str) do { for (const char *_p = (str); *_p; _p++) PUT(*_p); } while (0)
#define PUTN(v, w) do { char _b[16]; snprintf(_b, sizeof _b, "%0*d", (w), (v)); \
                        PUTS(_b); } while (0)

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        p++;
        switch (*p) {
        case 'a': PUTS(wdays[(tm->tm_wday >= 0 && tm->tm_wday < 7)
                             ? tm->tm_wday : 0]); break;
        case 'b': PUTS(months[(tm->tm_mon >= 0 && tm->tm_mon < 12)
                              ? tm->tm_mon : 0]); break;
        case 'd': PUTN(tm->tm_mday, 2); break;
        case 'e': { char b[8]; snprintf(b, sizeof b, "%2d", tm->tm_mday);
                    PUTS(b); } break;
        case 'H': PUTN(tm->tm_hour, 2); break;
        case 'M': PUTN(tm->tm_min, 2); break;
        case 'S': PUTN(tm->tm_sec, 2); break;
        case 'm': PUTN(tm->tm_mon + 1, 2); break;
        case 'Y': PUTN(tm->tm_year + 1900, 4); break;
        case 'y': PUTN((tm->tm_year + 1900) % 100, 2); break;
        case 'j': PUTN(tm->tm_yday + 1, 3); break;
        case 'Z': PUTS("GMT"); break;
        case '%': PUT('%'); break;
        case 0:   PUT('%'); p--; break;
        default:  PUT('%'); PUT(*p); break;
        }
    }
#undef PUTN
#undef PUTS
#undef PUT

    if (n < max) { s[n] = 0; return n; }
    if (max) s[max - 1] = 0;
    return 0;
}

/* Kept so that a caller asking whether this is a leap year gets an answer
 * from the same arithmetic everything else here uses. */
int __libc_is_leap_year(long y) { return leap(y); }
