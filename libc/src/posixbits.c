/* The small POSIX pieces that ported code reaches for.
 *
 * Each of these is here because something asked for it by name and would
 * otherwise not link. None of them needs anything this system does not have,
 * which is why they are written rather than stubbed: a strtok that returned
 * NULL would compile and then lose data.
 */

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* --- splitting a string -------------------------------------------------- */

static int in_set(char c, const char *set) {
    for (; *set; set++) if (*set == c) return 1;
    return 0;
}

char *strtok_r(char *s, const char *sep, char **save) {
    if (s == NULL) s = *save;
    if (s == NULL) return NULL;

    while (*s && in_set(*s, sep)) s++;      /* leading separators are skipped */
    if (*s == 0) { *save = s; return NULL; }

    char *start = s;
    while (*s && !in_set(*s, sep)) s++;
    if (*s) { *s = 0; s++; }                /* cut, and go on after the cut */
    *save = s;
    return start;
}

/* The saved position is one global. That is what the interface is, and why
 * strtok_r exists. */
static char *strtok_state;

char *strtok(char *s, const char *sep) {
    return strtok_r(s, sep, &strtok_state);
}

/* --- leaving ------------------------------------------------------------- */

/* The standard asks for at least 32. Library tidy-up routines register here
 * and there is no reason to be stingy. */
#define ATEXIT_MAX 32

static void (*atexit_fns[ATEXIT_MAX])(void);
static int   atexit_n;

int atexit(void (*fn)(void)) {
    if (fn == NULL || atexit_n >= ATEXIT_MAX) return -1;
    atexit_fns[atexit_n++] = fn;
    return 0;
}

/* Called from exit(), in reverse order of registration, as the standard
 * requires: what was set up last is torn down first. The count is dropped
 * before the call, so a handler that calls exit() again finishes the list
 * rather than starting it over. */
void __libc_run_atexit(void) {
    while (atexit_n > 0) {
        void (*fn)(void) = atexit_fns[--atexit_n];
        fn();
    }
}

/* --- the lenient address parser ------------------------------------------ */

/* inet_aton takes forms inet_pton refuses, and has since 4.2BSD: one, two,
 * three or four parts, each decimal, octal with a leading zero, or hex with
 * a leading 0x, and the last part filling all the bytes it has left. Programs
 * do rely on it -- "127.1" is the classic. Returns 1 if it understood. */
int inet_aton(const char *src, struct in_addr *dst) {
    if (src == NULL || dst == NULL) return 0;

    unsigned long parts[4];
    int n = 0;

    for (;;) {
        if (*src < '0' || *src > '9') return 0;

        int base = 10;
        if (*src == '0') {
            src++;
            if (*src == 'x' || *src == 'X') { base = 16; src++; }
            else base = 8;
            /* A lone "0" is zero in any base. */
        }

        unsigned long v = 0;
        int digits = (base == 10) ? 0 : 1;   /* the leading 0 already counts */
        for (;;) {
            int d;
            char c = *src;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (d >= base) break;
            v = v * (unsigned long)base + (unsigned long)d;
            if (v > 0xFFFFFFFFUL) return 0;
            src++;
            digits++;
        }
        if (digits == 0) return 0;

        if (n >= 4) return 0;
        parts[n++] = v;

        if (*src != '.') break;
        src++;
    }
    if (*src != 0) return 0;

    /* The last part fills whatever bytes the earlier ones left. */
    unsigned long addr;
    unsigned long last = parts[n - 1];
    unsigned long limit = (n == 1) ? 0xFFFFFFFFUL
                        : (n == 2) ? 0x00FFFFFFUL
                        : (n == 3) ? 0x0000FFFFUL
                                   : 0x000000FFUL;
    if (last > limit) return 0;

    addr = last;
    for (int i = 0; i < n - 1; i++) {
        if (parts[i] > 255) return 0;
        addr |= parts[i] << (24 - i * 8);
    }

    dst->s_addr = htonl((unsigned int)addr);
    return 1;
}
