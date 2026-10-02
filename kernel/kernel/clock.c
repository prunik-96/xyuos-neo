// The wall clock and the time zone. See clock.h.

#include "clock.h"
#include "kio.h"
#include "../arch/x86_64/pit.h"
#include "../fs/vfs.h"
#include "../net/net.h"

#define CLOCK_FILE "/home/.clock"

// --- the zones ------------------------------------------------------------------
//
// A table, not the tz database: the offsets people live by, and the two
// summer-time rules still in use around them. Russia, Belarus, Kazakhstan and
// most of Asia keep no summer time at all.

enum { DST_NONE, DST_EU, DST_US };

struct zone { const char *name, *ru, *en; int16_t std_min; uint8_t rule; };

static const struct zone zones[] = {
    { "UTC",                 "UTC",                         "UTC",                         0,    DST_NONE },
    { "Europe/London",       "Лондон, Лиссабон",            "London, Lisbon",              0,    DST_EU },
    { "Europe/Berlin",       "Берлин, Париж, Варшава",      "Berlin, Paris, Warsaw",       60,   DST_EU },
    { "Europe/Prague",       "Прага, Вена, Рим",            "Prague, Vienna, Rome",        60,   DST_EU },
    { "Europe/Kyiv",         "Киев",                        "Kyiv",                        120,  DST_EU },
    { "Europe/Riga",         "Рига, Вильнюс, Таллин",       "Riga, Vilnius, Tallinn",      120,  DST_EU },
    { "Europe/Helsinki",     "Хельсинки",                   "Helsinki",                    120,  DST_EU },
    { "Europe/Chisinau",     "Кишинёв",                     "Chisinau",                    120,  DST_EU },
    { "Europe/Kaliningrad",  "Калининград",                 "Kaliningrad",                 120,  DST_NONE },
    { "Europe/Minsk",        "Минск",                       "Minsk",                       180,  DST_NONE },
    { "Europe/Moscow",       "Москва, Санкт-Петербург",     "Moscow, St Petersburg",       180,  DST_NONE },
    { "Europe/Istanbul",     "Стамбул",                     "Istanbul",                    180,  DST_NONE },
    { "Europe/Samara",       "Самара, Ижевск",              "Samara, Izhevsk",             240,  DST_NONE },
    { "Asia/Tbilisi",        "Тбилиси",                     "Tbilisi",                     240,  DST_NONE },
    { "Asia/Yerevan",        "Ереван",                      "Yerevan",                     240,  DST_NONE },
    { "Asia/Baku",           "Баку",                        "Baku",                        240,  DST_NONE },
    { "Asia/Dubai",          "Дубай",                       "Dubai",                       240,  DST_NONE },
    { "Asia/Yekaterinburg",  "Екатеринбург, Пермь",         "Yekaterinburg, Perm",         300,  DST_NONE },
    { "Asia/Tashkent",       "Ташкент",                     "Tashkent",                    300,  DST_NONE },
    { "Asia/Almaty",         "Алматы, Астана",              "Almaty, Astana",              300,  DST_NONE },
    { "Asia/Kolkata",        "Дели, Мумбаи",                "Delhi, Mumbai",               330,  DST_NONE },
    { "Asia/Omsk",           "Омск",                        "Omsk",                        360,  DST_NONE },
    { "Asia/Bishkek",        "Бишкек",                      "Bishkek",                     360,  DST_NONE },
    { "Asia/Novosibirsk",    "Новосибирск, Красноярск",     "Novosibirsk, Krasnoyarsk",    420,  DST_NONE },
    { "Asia/Bangkok",        "Бангкок, Ханой",              "Bangkok, Hanoi",              420,  DST_NONE },
    { "Asia/Irkutsk",        "Иркутск",                     "Irkutsk",                     480,  DST_NONE },
    { "Asia/Shanghai",       "Пекин, Шанхай",               "Beijing, Shanghai",           480,  DST_NONE },
    { "Asia/Yakutsk",        "Якутск, Чита",                "Yakutsk, Chita",              540,  DST_NONE },
    { "Asia/Tokyo",          "Токио, Сеул",                 "Tokyo, Seoul",                540,  DST_NONE },
    { "Asia/Vladivostok",    "Владивосток, Хабаровск",      "Vladivostok, Khabarovsk",     600,  DST_NONE },
    { "Asia/Magadan",        "Магадан, Сахалин",            "Magadan, Sakhalin",           660,  DST_NONE },
    { "Asia/Kamchatka",      "Петропавловск-Камчатский",    "Kamchatka",                   720,  DST_NONE },
    { "America/New_York",    "Нью-Йорк, Торонто",           "New York, Toronto",           -300, DST_US },
    { "America/Chicago",     "Чикаго",                      "Chicago",                     -360, DST_US },
    { "America/Denver",      "Денвер",                      "Denver",                      -420, DST_US },
    { "America/Los_Angeles", "Лос-Анджелес, Ванкувер",      "Los Angeles, Vancouver",      -480, DST_US },
};
#define NZONES ((int)(sizeof zones / sizeof zones[0]))
#define DEFAULT_ZONE 10                      // Europe/Moscow

static int zone = DEFAULT_ZONE;

// --- the calendar ---------------------------------------------------------------

static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

// 0 = Sunday. The first of January 1970 was a Thursday.
static int weekday(int64_t days) { return (int)(((days % 7) + 11) % 7); }

static int64_t last_sunday(int y, int m) {       // m has 31 days (March, October)
    int64_t d = days_from_civil(y, m, 31);
    return d - weekday(d);
}

static int64_t nth_sunday(int y, int m, int n) {
    int64_t d = days_from_civil(y, m, 1);
    d += (7 - weekday(d)) % 7;
    return d + 7 * (n - 1);
}

void clock_tm(int64_t t, struct rtc_time *out) {
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t s = t - days * 86400;
    civil_from_days(days, &out->year, &out->mon, &out->day);
    out->hour = (int)(s / 3600);
    out->min = (int)(s / 60 % 60);
    out->sec = (int)(s % 60);
}

static int dst_at(int z, int64_t utc) {
    if (zones[z].rule == DST_NONE) return 0;
    struct rtc_time t;
    clock_tm(utc, &t);
    int64_t from, to;
    if (zones[z].rule == DST_EU) {
        // The last Sunday of March to the last of October, 01:00 UTC both.
        from = last_sunday(t.year, 3) * 86400 + 3600;
        to = last_sunday(t.year, 10) * 86400 + 3600;
    } else {
        // The second Sunday of March, 02:00 local standard time, to the first
        // of November, 02:00 local summer time.
        int64_t std = (int64_t)zones[z].std_min * 60;
        from = nth_sunday(t.year, 3, 2) * 86400 + 7200 - std;
        to = nth_sunday(t.year, 11, 1) * 86400 + 7200 - std - 3600;
    }
    return utc >= from && utc < to;
}

int clock_is_dst(int64_t utc) { return dst_at(zone, utc); }

int clock_offset(int64_t utc) {
    return zones[zone].std_min * 60 + (dst_at(zone, utc) ? 3600 : 0);
}

// --- the clock --------------------------------------------------------------------

static int64_t rtc_boot;          // the RTC at boot, read as if it were UTC
static uint64_t anchor_us;        // the counter at the anchor
static int64_t anchor_ms;         // UTC at the anchor; valid once `anchored`
static int anchored;              // set from NTP (this boot)

static struct clock_state st;

// What the RTC is taken to be off by: what NTP taught, else "local time".
static int64_t rtc_skew_now(void) {
    if (st.rtc_known) return st.rtc_skew;
    return clock_offset(rtc_boot - zones[zone].std_min * 60);
}

void clock_init(void) {
    rtc_boot = rtc_now_unix();
    anchor_us = pit_now_us();
}

int64_t clock_utc_ms(void) {
    uint64_t now = pit_now_us();
    int64_t since = (int64_t)((now - anchor_us) / 1000);
    if (anchored) return anchor_ms + since;
    return (rtc_boot - rtc_skew_now()) * 1000 + since;
}

int64_t clock_utc(void) {
    int64_t ms = clock_utc_ms();
    return ms >= 0 ? ms / 1000 : (ms - 999) / 1000;
}

void clock_local_tm(struct rtc_time *out) {
    int64_t t = clock_utc();
    clock_tm(t + clock_offset(t), out);
}

// --- the file ---------------------------------------------------------------------
//
// zone=NAME, and once learnt rtc=SECONDS (the board's clock minus UTC) and
// sync=SECONDS (when, UTC).

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int put_str(char *b, int n, const char *s) { while (*s) b[n++] = *s++; return n; }

static int put_num(char *b, int n, int64_t v) {
    char t[24];
    int k = 0;
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[n++] = '-';
    while (k) b[n++] = t[--k];
    return n;
}

static void clock_save(void) {
    char buf[160];
    int n = 0;
    n = put_str(buf, n, "zone=");
    n = put_str(buf, n, zones[zone].name);
    buf[n++] = '\n';
    if (st.rtc_known) {
        n = put_str(buf, n, "rtc=");
        n = put_num(buf, n, st.rtc_skew);
        buf[n++] = '\n';
    }
    if (st.sync_utc) {
        n = put_str(buf, n, "sync=");
        n = put_num(buf, n, st.sync_utc);
        buf[n++] = '\n';
    }
    vfs_unlink(CLOCK_FILE);
    if (vfs_create(CLOCK_FILE) != 0) return;
    int fd = vfs_open(CLOCK_FILE);
    if (fd < 0) return;
    vfs_write(fd, buf, (uint32_t)n);
    vfs_close(fd);
}

static int find_zone(const char *name) {
    for (int i = 0; i < NZONES; i++) if (str_eq(zones[i].name, name)) return i;
    return -1;
}

void clock_load(void) {
    char buf[200];
    int fd = vfs_open(CLOCK_FILE);
    if (fd < 0) return;
    int n = vfs_read(fd, buf, sizeof buf - 1);
    vfs_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; *line; ) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char save = *end;
        *end = 0;
        char *eq = line;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = 0;
            const char *v = eq + 1;
            int64_t num = 0;
            int neg = *v == '-';
            for (const char *c = v + neg; *c >= '0' && *c <= '9'; c++) num = num * 10 + (*c - '0');
            if (neg) num = -num;
            if (str_eq(line, "zone")) { int z = find_zone(v); if (z >= 0) zone = z; }
            else if (str_eq(line, "rtc")) { st.rtc_known = 1; st.rtc_skew = (int)num; }
            else if (str_eq(line, "sync")) st.sync_utc = num;
        }
        *end = save;
        line = *end ? end + 1 : end;
    }
    struct rtc_time t;
    clock_local_tm(&t);
    kprintf("clock: zone %s, board clock %s, now %d-%d-%d %d:%d local\n", zones[zone].name,
            st.rtc_known ? "known" : "taken as local time", t.year, t.mon, t.day, t.hour, t.min);
}

// --- zones ------------------------------------------------------------------------

int clock_set_zone(const char *name) {
    int z = find_zone(name);
    if (z < 0) return -1;
    // Before NTP, the clock leans on the RTC being local time -- in the OLD
    // zone. Keep it where it is: what changes is how it is shown, not when it is.
    if (!anchored) {
        anchor_ms = clock_utc_ms();
        anchor_us = pit_now_us();
        anchored = 1;
    }
    zone = z;
    clock_save();
    return 0;
}

const char *clock_zone_name(void) { return zones[zone].name; }
int clock_zone_count(void) { return NZONES; }

int clock_zone_get(int i, const char **name, const char **ru, const char **en, int *std_min, int *rule) {
    if (i < 0 || i >= NZONES) return -1;
    if (name) *name = zones[i].name;
    if (ru) *ru = zones[i].ru;
    if (en) *en = zones[i].en;
    if (std_min) *std_min = zones[i].std_min;
    if (rule) *rule = zones[i].rule;
    return 0;
}

int clock_zone_offset_now(int i) {
    if (i < 0 || i >= NZONES) return 0;
    int64_t now = clock_utc();
    return zones[i].std_min * 60 + (dst_at(i, now) ? 3600 : 0);
}

void clock_state(struct clock_state *out) { *out = st; }

// --- NTP --------------------------------------------------------------------------

static void str_copy(char *d, const char *s, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

int clock_sync(const char *server, int quick) {
    if (!net_is_up()) return -1;
    static const char *pool[] = { "pool.ntp.org", "time.google.com", "time.cloudflare.com" };
    const char *list[4];
    int n = 0;
    if (server && server[0]) list[n++] = server;
    for (int i = 0; i < 3 && !(quick && n); i++) list[n++] = pool[i];

    for (int i = 0; i < n; i++) {
        uint32_t ip;
        if (!net_parse_ip(list[i], &ip) && !net_resolve(list[i], &ip)) continue;
        int64_t utc_ms;
        uint64_t at_us;
        uint32_t rtt;
        if (!net_ntp(ip, quick ? 2 : 3, &utc_ms, &at_us, &rtt)) continue;

        // How far off we were, at the very instant the answer arrived.
        int64_t was = (anchored ? anchor_ms + (int64_t)(at_us - anchor_us) / 1000
                                : (rtc_boot - rtc_skew_now()) * 1000 + (int64_t)(at_us - anchor_us) / 1000);
        anchor_ms = utc_ms;
        anchor_us = at_us;
        anchored = 1;

        st.synced = 1;
        st.delta_ms = (int)(utc_ms - was);
        st.rtt_ms = (int)rtt;
        st.sync_utc = utc_ms / 1000;
        // What the board's clock holds, now that the truth is known: read it
        // again (it has been ticking all along) and compare.
        st.rtc_skew = (int)(rtc_now_unix() - clock_utc());
        st.rtc_known = 1;
        str_copy(st.server, list[i], sizeof st.server);
        clock_save();
        kprintf("clock: set from %s, was off by %d ms; the board clock is UTC%s%d s\n",
                list[i], st.delta_ms, st.rtc_skew >= 0 ? "+" : "", st.rtc_skew);
        return 1;
    }
    return 0;
}
