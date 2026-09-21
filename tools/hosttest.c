/* Host harness: compile the kernel's certificate code with an ordinary gcc and
 * point it at real certificates, so a parse failure can be found in a second
 * instead of a boot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "x509.h"

/* --- stubs for the kernel bits x509.c leans on --------------------------- */
void kprintf(const char *fmt, ...) { (void)fmt; }

int64_t rtc_to_unix(const struct rtc_time *t) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = t->year - 1900;
    tm.tm_mon = t->mon - 1;
    tm.tm_mday = t->day;
    tm.tm_hour = t->hour;
    tm.tm_min = t->min;
    tm.tm_sec = t->sec;
    return (int64_t)timegm(&tm);
}
int64_t rtc_now_unix(void) { return (int64_t)time(NULL); }
void rtc_read(struct rtc_time *o) { (void)o; }

/* --- the test ------------------------------------------------------------ */

static uint8_t *slurp(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    static uint8_t buf[1 << 20];
    *len = (int)fread(buf, 1, sizeof buf, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s HOST cert.der [cert.der ...]\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    static const uint8_t *certs[8];
    static int lens[8];
    int n = 0;

    for (int i = 2; i < argc && n < 8; i++, n++) {
        int l;
        uint8_t *d = malloc(1 << 20);
        int tmp;
        uint8_t *src = slurp(argv[i], &tmp);
        memcpy(d, src, tmp);
        l = tmp;
        certs[n] = d;
        lens[n] = l;

        x509_t c;
        if (!x509_parse(d, l, &c)) {
            printf("cert %d (%s): PARSE FAILED, %d bytes\n", n, argv[i], l);
            continue;
        }
        printf("cert %d (%s): %d bytes  sig_alg=%d key_alg=%d is_ca=%d san=%d\n",
               n, argv[i], l, c.sig_alg, c.key_alg, c.is_ca, c.sanlen);
        printf("            valid %lld .. %lld  (now %lld)\n",
               (long long)c.not_before, (long long)c.not_after,
               (long long)rtc_now_unix());
        if (n == 0)
            printf("            host match: %d\n", x509_host_matches(&c, host));
    }

    const char *why = "";
    int ok = x509_verify_chain(certs, lens, n, host, &why);
    printf("\nchain: %s%s%s\n", ok ? "VERIFIED" : "REJECTED",
           ok ? "" : " -- ", ok ? "" : why);
    return ok ? 0 : 1;
}
