#!/usr/bin/env python3
"""Declare what libc/src/timecal.c now defines, and add the small POSIX
pieces NetSurf's core reaches for. One-off; kept only so the change is
readable."""
import os

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))
R = XYUOS + "/"


def edit(path, tag, a, b, marker):
    s = open(R + path).read()
    if marker in s:
        print("already: %s (%s)" % (path, tag))
        return
    if a not in s:
        raise SystemExit("MISSING in %s [%s]:\n%r" % (path, tag, a[:200]))
    open(R + path, "w").write(s.replace(a, b, 1))
    print("ok: %s (%s)" % (path, tag))


edit("libc/include/time.h", "the calendar",
     """time_t     time(time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);""",
     """time_t     time(time_t *t);

/* Real conversions, in both directions: mktime(gmtime(t)) gives back t. The
 * clock this system starts from is wrong; arithmetic on it is not. There is
 * no timezone database here and no way to learn one, so local time is UTC. */
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);
time_t     mktime(struct tm *tm);
double     difftime(time_t a, time_t b);

char      *asctime(const struct tm *tm);
char      *ctime(const time_t *t);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);""",
     "mktime")

edit("libc/include/ctype.h", "isascii",
     "static inline int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }",
     """static inline int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
static inline int isascii(int c)  { return c >= 0 && c < 128; }
static inline int toascii(int c)  { return c & 0x7F; }""",
     "isascii")

edit("libc/include/sys/time.h", "the timeval macros",
     "int gettimeofday(struct timeval *tv, void *tz);",
     """int gettimeofday(struct timeval *tv, void *tz);

/* The comparisons that go with it. Portable code writes these whether or not
 * it ever waits on anything, and they are arithmetic on a struct, so there is
 * nothing here for this system to be unable to do. */
#define timerisset(a)      ((a)->tv_sec || (a)->tv_usec)
#define timerclear(a)      ((a)->tv_sec = (a)->tv_usec = 0)
#define timercmp(a, b, op) \\
        (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec op (b)->tv_usec) \\
                                      : ((a)->tv_sec  op (b)->tv_sec))
#define timeradd(a, b, r)  do {                                  \\
        (r)->tv_sec  = (a)->tv_sec  + (b)->tv_sec;               \\
        (r)->tv_usec = (a)->tv_usec + (b)->tv_usec;              \\
        if ((r)->tv_usec >= 1000000) { (r)->tv_sec++;            \\
                                       (r)->tv_usec -= 1000000; }\\
    } while (0)
#define timersub(a, b, r)  do {                                  \\
        (r)->tv_sec  = (a)->tv_sec  - (b)->tv_sec;               \\
        (r)->tv_usec = (a)->tv_usec - (b)->tv_usec;              \\
        if ((r)->tv_usec < 0) { (r)->tv_sec--;                   \\
                                (r)->tv_usec += 1000000; }       \\
    } while (0)""",
     "timerisset")

edit("libc/include/string.h", "strtok",
     "char *strdup(const char *s);",
     """char  *strdup(const char *s);

/* Splits a string in place, remembering where it got to between calls. The
 * saved position is one global, which is what the interface says it is; the
 * _r form takes its own and is the one to reach for. */
char  *strtok(char *s, const char *sep);
char  *strtok_r(char *s, const char *sep, char **save);""",
     "strtok")

edit("libc/include/stdlib.h", "atexit",
     "void exit(int code) __attribute__((noreturn));",
     """void exit(int code) __attribute__((noreturn));

/* Run on the way out, most recently registered first. Library code registers
 * a tidy-up here and would otherwise not link at all. */
int  atexit(void (*fn)(void));""",
     "atexit")

edit("libc/include/arpa/inet.h", "inet_aton",
     "int inet_pton(int af, const char *src, void *dst);",
     """int inet_pton(int af, const char *src, void *dst);

/* The older, lenient form: it accepts \"10.1\" and \"0x7f000001\" as well as a
 * full four-part address, because that is what it has always accepted and
 * callers rely on it. Returns 1 for an address it understood, 0 otherwise. */
int inet_aton(const char *src, struct in_addr *dst);""",
     "inet_aton")

print("--- headers done")
