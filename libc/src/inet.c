/* Addresses, the system's name, the locale there is one of, and a random
 * number generator -- the odds and ends a portable program expects to find
 * and this libc had not got.
 */
#include <arpa/inet.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

/* --- addresses as text ------------------------------------------------- */

/* Four numbers, each 0..255, separated by three dots and nothing else. Every
 * refusal here is a hostname somewhere, so the rules are kept strict: a
 * leading zero, a fifth part or a trailing character all mean "not a
 * number", and the caller goes and looks the name up instead. */
static int pton4(const char *s, unsigned char out[4]) {
    int part = 0;
    for (;;) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, digits = 0, lead0 = (*s == '0');
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            if (++digits > 3 || v > 255) return 0;
        }
        if (lead0 && digits > 1) return 0;   /* 010 is not ten anywhere */
        out[part++] = (unsigned char)v;
        if (part == 4) return *s == 0;
        if (*s++ != '.') return 0;
    }
}

/* Colons, hex, and at most one "::" standing for the zeroes left out. */
static int pton6(const char *s, unsigned char out[16]) {
    unsigned char buf[16];
    memset(buf, 0, sizeof buf);
    int at = 0, gap = -1;

    if (s[0] == ':') {
        if (s[1] != ':') return 0;
        s += 1;
    }

    while (*s) {
        if (*s == ':') {
            if (gap >= 0) return 0;          /* only one :: is allowed */
            gap = at;
            s++;
            if (*s == 0) break;
            continue;
        }

        /* A group, or the tail written as a dotted quad. */
        const char *dot = s;
        while (*dot && *dot != ':') dot++;
        int is_v4 = 0;
        for (const char *c = s; c < dot; c++)
            if (*c == '.') { is_v4 = 1; break; }

        if (is_v4) {
            if (at > 12) return 0;
            unsigned char q[4];
            if (!pton4(s, q)) return 0;
            memcpy(buf + at, q, 4);
            at += 4;
            s = dot;
            break;
        }

        unsigned v = 0;
        int digits = 0;
        while (s < dot) {
            int c = *s++;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = v * 16 + (unsigned)d;
            if (++digits > 4) return 0;
        }
        if (at > 14) return 0;
        buf[at++] = (unsigned char)(v >> 8);
        buf[at++] = (unsigned char)(v & 0xFF);
        if (*s == ':') s++;
        else if (*s) return 0;
    }

    if (gap >= 0) {
        if (at == 16) return 0;              /* nothing was left out */
        int tail = at - gap;
        memmove(buf + 16 - tail, buf + gap, (size_t)tail);
        memset(buf + gap, 0, (size_t)(16 - tail - gap));
    } else if (at != 16) {
        return 0;
    }

    memcpy(out, buf, 16);
    return 1;
}

int inet_pton(int af, const char *src, void *dst) {
    if (!src || !dst) return 0;
    if (af == AF_INET) {
        unsigned char q[4];
        if (!pton4(src, q)) return 0;
        memcpy(dst, q, 4);
        return 1;
    }
    if (af == AF_INET6) {
        unsigned char a[16];
        if (!pton6(src, a)) return 0;
        memcpy(dst, a, 16);
        return 1;
    }
    return -1;
}

static char *put_uint(char *p, unsigned v) {
    char tmp[8];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    const unsigned char *b = (const unsigned char *)src;
    char tmp[INET6_ADDRSTRLEN];
    char *p = tmp;

    if (!src || !dst) return 0;

    if (af == AF_INET) {
        for (int i = 0; i < 4; i++) {
            if (i) *p++ = '.';
            p = put_uint(p, b[i]);
        }
        *p = 0;
    } else if (af == AF_INET6) {
        /* The longest run of zero groups is written as "::", and only the
         * longest -- two of them would be ambiguous. */
        int best = -1, bestlen = 0, run = -1, runlen = 0;
        unsigned g[8];
        for (int i = 0; i < 8; i++) g[i] = ((unsigned)b[i*2] << 8) | b[i*2+1];
        for (int i = 0; i < 8; i++) {
            if (g[i] == 0) {
                if (run < 0) { run = i; runlen = 0; }
                if (++runlen > bestlen) { best = run; bestlen = runlen; }
            } else {
                run = -1;
            }
        }
        if (bestlen < 2) best = -1;

        for (int i = 0; i < 8; ) {
            if (i == best) {
                *p++ = ':';
                if (best == 0) *p++ = ':';
                i += bestlen;
                if (i == 8) *p++ = ':';
                continue;
            }
            if (i) *p++ = ':';
            unsigned v = g[i++];
            static const char hex[] = "0123456789abcdef";
            int started = 0;
            for (int sh = 12; sh >= 0; sh -= 4) {
                int d = (int)((v >> sh) & 0xF);
                if (d || started || sh == 0) { *p++ = hex[d]; started = 1; }
            }
        }
        *p = 0;
    } else {
        return 0;
    }

    if ((socklen_t)(p - tmp) + 1 > size) return 0;
    memcpy(dst, tmp, (size_t)(p - tmp) + 1);
    return dst;
}

/* --- the system's name --------------------------------------------------- */

int uname(struct utsname *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->sysname, sizeof out->sysname, "xyuOS");
    snprintf(out->nodename, sizeof out->nodename, "xyuos");
    snprintf(out->release, sizeof out->release, "Neo");
    snprintf(out->version, sizeof out->version, __DATE__);
    snprintf(out->machine, sizeof out->machine, "x86_64");
    return 0;
}

/* --- the locale there is one of ------------------------------------------ */

static struct lconv c_locale = {
    ".", "", "", "", "", ".", "", "", "", "",
    127, 127, 127, 127, 127, 127, 127, 127
};

char *setlocale(int category, const char *locale) {
    (void)category;
    /* Asking, or asking for the only one there is. */
    if (!locale || !locale[0] || !strcmp(locale, "C") || !strcmp(locale, "POSIX"))
        return (char *)"C";
    return 0;
}

struct lconv *localeconv(void) { return &c_locale; }

/* Signals were a polite lie here -- remembered and never delivered. They are
 * real now and live in signal.c. */

/* --- random numbers ------------------------------------------------------ */

/* The usual small linear congruential generator. Nothing here should be
 * trusted with anything that matters -- it is for shuffling and for jitter,
 * which is what callers of rand() actually want. */
static unsigned long rand_state = 1;

void srand(unsigned seed) { rand_state = seed; }

int rand(void) {
    rand_state = rand_state * 6364136223846793005UL + 1442695040888963407UL;
    return (int)((rand_state >> 33) & 0x7FFFFFFF);
}
