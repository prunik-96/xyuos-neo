/* Waiting on descriptors.
 *
 * The types are here because portable code declares an fd_set on its way past
 * whether or not it ever waits on one. select() itself is declared and not
 * written: this system has no descriptor to wait on that would not already
 * have answered, and a select() that returned a lie would be worse than a
 * link error naming the file that wanted it.
 */
#ifndef SYS_SELECT_H
#define SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>

#define FD_SETSIZE 64

typedef struct {
    unsigned long fds_bits[(FD_SETSIZE + 63) / 64];
} fd_set;

#define FD_ZERO(s)    do { \
        for (unsigned _i = 0; _i < sizeof((s)->fds_bits) / sizeof(unsigned long); _i++) \
            (s)->fds_bits[_i] = 0; \
    } while (0)
#define FD_SET(f, s)   ((s)->fds_bits[(f) / 64] |=  (1UL << ((f) % 64)))
#define FD_CLR(f, s)   ((s)->fds_bits[(f) / 64] &= ~(1UL << ((f) % 64)))
#define FD_ISSET(f, s) (((s)->fds_bits[(f) / 64] >> ((f) % 64)) & 1UL)

#ifdef __cplusplus
extern "C" {
#endif

/* Declared so that code mentioning it compiles; not written, so that code
 * calling it says so at link time instead of at runtime. */
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);

#ifdef __cplusplus
}
#endif

#endif
