#ifndef POLL_H
#define POLL_H

/* Waiting on several descriptors at once.
 *
 * There is nothing to wait for here: every file on this system is a file on a
 * local disk, and a read from one either succeeds or fails immediately. So
 * poll() answers straight away that everything is ready, which for a set of
 * ordinary files is not a lie -- it is what a real poll() would also say.
 *
 * The header exists mainly for the constants. Ported code uses POLLIN and
 * POLLOUT to describe what it wants long before it ever calls poll(). */

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif
