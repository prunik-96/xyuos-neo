#ifndef SYS_TIME_H
#define SYS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    long tv_sec;
    long tv_usec;
};

int gettimeofday(struct timeval *tv, void *tz);

/* The comparisons that go with it. Portable code writes these whether or not
 * it ever waits on anything, and they are arithmetic on a struct, so there is
 * nothing here for this system to be unable to do. */
#define timerisset(a)      ((a)->tv_sec || (a)->tv_usec)
#define timerclear(a)      ((a)->tv_sec = (a)->tv_usec = 0)
#define timercmp(a, b, op) \
        (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec op (b)->tv_usec) \
                                      : ((a)->tv_sec  op (b)->tv_sec))
#define timeradd(a, b, r)  do {                                  \
        (r)->tv_sec  = (a)->tv_sec  + (b)->tv_sec;               \
        (r)->tv_usec = (a)->tv_usec + (b)->tv_usec;              \
        if ((r)->tv_usec >= 1000000) { (r)->tv_sec++;            \
                                       (r)->tv_usec -= 1000000; }\
    } while (0)
#define timersub(a, b, r)  do {                                  \
        (r)->tv_sec  = (a)->tv_sec  - (b)->tv_sec;               \
        (r)->tv_usec = (a)->tv_usec - (b)->tv_usec;              \
        if ((r)->tv_usec < 0) { (r)->tv_sec--;                   \
                                (r)->tv_usec += 1000000; }       \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif
