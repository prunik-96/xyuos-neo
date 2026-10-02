#ifndef POLL_H
#define POLL_H

/* Waiting on several descriptors at once.
 *
 * Sockets are really waited on: until one has something to read, room to
 * write, a connection to accept, or has ended. A file is always ready -- a
 * read from one never waits -- and so is the terminal, which is the one
 * thing this cannot see into (a read from it waits for a whole line). */

#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020
#define POLLRDNORM 0x0040
#define POLLWRNORM 0x0100

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#ifdef __cplusplus
extern "C" {
#endif

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif

#endif
