/* POSIX-shaped file descriptor layer, plus the odds and ends a C compiler
 * expects from a hosted environment.
 *
 * The kernel hands out descriptors starting at 0, but POSIX code reserves
 * 0/1/2 for stdin/stdout/stderr -- so this layer offsets every real file
 * descriptor by FD_OFFSET. Everything above this line therefore sees normal
 * POSIX numbering, and `write(1, ...)` is unambiguous.
 */

#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/mman.h>

#define FD_OFFSET 3

int open(const char *path, int flags, ...) {
    if (flags & O_CREAT) xyuos_create(path);     /* ignore "already exists" */

    long kfd = xyuos_open(path);
    if (kfd < 0) return -1;

    if (flags & O_TRUNC)  xyuos_ftruncate(kfd, 0);
    if (flags & O_APPEND) xyuos_seek(kfd, 0, SEEK_END);
    return (int)(kfd + FD_OFFSET);
}

long read(int fd, void *buf, unsigned long len) {
    /* stdin: the kernel decides what that means -- a redirected file if the
     * shell set one up, otherwise a keypress. Returns 0 at end of input, which
     * is what makes `prog < file` terminate. */
    if (fd == 0) return xyuos_read_stdin(buf, len);
    if (fd < FD_OFFSET) return -1;
    return xyuos_read(fd - FD_OFFSET, buf, len);
}

long write(int fd, const void *buf, unsigned long len) {
    if (fd == 1 || fd == 2) return xyuos_console_write(buf, len);
    if (fd < FD_OFFSET) return -1;
    return xyuos_writefd(fd - FD_OFFSET, buf, len);
}

int close(int fd) {
    if (fd < FD_OFFSET) return 0;                 /* closing a std stream is a no-op */
    xyuos_close(fd - FD_OFFSET);
    return 0;
}

long lseek(int fd, long offset, int whence) {
    if (fd < FD_OFFSET) return -1;
    return xyuos_seek(fd - FD_OFFSET, offset, whence);
}

int unlink(const char *path) { return (int)xyuos_unlink(path); }

/* --- paths ---------------------------------------------------------------
 * Userland has no per-process working directory: the shell keeps its cwd in
 * the kernel, per pane, and passes absolute paths down. So "where am I" is
 * always the root here.
 */
char *getcwd(char *buf, unsigned long size) {
    if (!buf || size < 2) return NULL;
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

char *realpath(const char *path, char *resolved) {
    if (!path || !resolved) return NULL;
    int i = 0;
    if (path[0] != '/') resolved[i++] = '/';       /* relative means from root */
    while (path[i ? i - 1 : 0] || i == 0) {
        char c = *path++;
        if (!c) break;
        resolved[i++] = c;
    }
    resolved[i] = '\0';
    return resolved;
}

/* --- process control -----------------------------------------------------
 * This OS runs one program at a time and cannot spawn another, so exec always
 * fails. tcc only reaches here when it wants to hand work to an external
 * assembler or linker -- which is exactly the path we avoid by using -run.
 */
int execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv;
    return -1;
}


int ftruncate(int fd, unsigned long length) {
    if (fd < FD_OFFSET) return -1;
    return (int)xyuos_ftruncate(fd - FD_OFFSET, length);
}

/* --- process environment -------------------------------------------------
 * There is no environment on this OS, but hosted code expects the symbol to
 * exist and to be a NULL-terminated vector, so give it a real empty one
 * rather than a null pointer that callers would walk off.
 */
static char *env_empty[1] = { NULL };
char **environ = env_empty;

/* --- assertions --------------------------------------------------------- */
void __assert_fail(const char *expr, const char *file, int line) {
    printf("assertion failed: %s at %s:%d\n", expr, file, line);
    abort();
}

/* --- time ---------------------------------------------------------------
 * There is no RTC driver yet, so the clock is fixed. tcc only needs this for
 * the __DATE__/__TIME__ macros and for -bench timings, both of which are
 * cosmetic; a fixed date is honest and predictable rather than pretending.
 */
#define FIXED_EPOCH 1752969600L   /* 2025-07-20 00:00:00 UTC */

time_t time(time_t *t) {
    if (t) *t = FIXED_EPOCH;
    return FIXED_EPOCH;
}

/* localtime, gmtime, mktime and the rest of the calendar are in timecal.c.
 * They used to be here, answering with a constant whatever they were asked;
 * that is fine until something does arithmetic on a date, and a browser does
 * nothing else with them. */

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) { tv->tv_sec = FIXED_EPOCH; tv->tv_usec = 0; }
    return 0;
}

/* --- math ---------------------------------------------------------------
 * Only what the compiler front end needs: scaling a mantissa by a power of
 * two while parsing floating-point literals.
 */
/* ldexp, fabs and their relatives moved to math.c, which is where the
 * rest of the elementary functions now live. */

double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    double value = 0.0;
    int any = 0;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {   /* hex float */
        p += 2;
        while (isxdigit((unsigned char)*p)) {
            int d = isdigit((unsigned char)*p) ? *p - '0'
                                               : (tolower((unsigned char)*p) - 'a' + 10);
            value = value * 16.0 + d;
            p++; any = 1;
        }
        if (*p == '.') {
            p++;
            double scale = 1.0 / 16.0;
            while (isxdigit((unsigned char)*p)) {
                int d = isdigit((unsigned char)*p) ? *p - '0'
                                                   : (tolower((unsigned char)*p) - 'a' + 10);
                value += d * scale;
                scale /= 16.0;
                p++; any = 1;
            }
        }
        if (*p == 'p' || *p == 'P') {
            p++;
            int esign = 1;
            if (*p == '-') { esign = -1; p++; } else if (*p == '+') p++;
            int e = 0;
            while (isdigit((unsigned char)*p)) { e = e * 10 + (*p - '0'); p++; }
            value = ldexp(value, esign * e);
        }
    } else {
        while (isdigit((unsigned char)*p)) { value = value * 10.0 + (*p - '0'); p++; any = 1; }
        if (*p == '.') {
            p++;
            double scale = 0.1;
            while (isdigit((unsigned char)*p)) {
                value += (*p - '0') * scale;
                scale /= 10.0;
                p++; any = 1;
            }
        }
        if (any && (*p == 'e' || *p == 'E')) {
            p++;
            int esign = 1;
            if (*p == '-') { esign = -1; p++; } else if (*p == '+') p++;
            int e = 0;
            while (isdigit((unsigned char)*p)) { e = e * 10 + (*p - '0'); p++; }
            while (e-- > 0) { if (esign > 0) value *= 10.0; else value /= 10.0; }
        }
    }

    if (end) *end = (char *)(any ? p : s);
    return neg ? -value : value;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }

/* Long double literals lose precision here: this returns a double widened to
 * long double rather than parsing at 80-bit precision. Fine for now; worth
 * revisiting if a program depends on full long double literals. */
long double strtold(const char *s, char **end) { return (long double)strtod(s, end); }

double atof(const char *s) { return strtod(s, NULL); }
