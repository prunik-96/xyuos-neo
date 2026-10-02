#ifndef SOCK_H
#define SOCK_H

#include <stdint.h>

// Sockets for programs: TCP and UDP over the stack in net.c, the way the
// Berkeley API has them. Each belongs to the program that opened it (its
// thread group) and is closed when that program ends, however it ends.
//
// Every call returns a count or 0, or a negated errno (the usual numbers).
// One that has to wait sleeps in between -- the rest of the machine runs --
// and gives up early for a signal (-EINTR), at the socket's timeout
// (-EAGAIN), or at once when the socket is non-blocking (-EAGAIN, and
// -EINPROGRESS for a connect).

#define SOCK_MAX 32

#define SK_STREAM 1
#define SK_DGRAM  2

// Options (sock_setopt / sock_getopt).
#define SKO_NONBLOCK  1
#define SKO_RCVTIMEO  2          // ms, 0 = forever
#define SKO_SNDTIMEO  3
#define SKO_ERROR     4          // read: the pending error, cleared
#define SKO_TYPE      5
#define SKO_NREAD     6          // read: bytes (or the next datagram's size) waiting

// poll() bits, as in <poll.h>.
#define SKP_IN   0x01
#define SKP_OUT  0x04
#define SKP_ERR  0x08
#define SKP_HUP  0x10
#define SKP_NVAL 0x20

#ifndef SOCK_POLLENT_DEFINED
#define SOCK_POLLENT_DEFINED
struct sock_pollent { int sid; short events, revents; };
#endif

int  sock_open(int type);
int  sock_bind(int sid, uint32_t ip, uint16_t port);
int  sock_listen(int sid, int backlog);
int  sock_accept(int sid, uint32_t *ip, uint16_t *port);
int  sock_connect(int sid, uint32_t ip, uint16_t port);
int  sock_send(int sid, const void *buf, int len, int flags, uint32_t ip, uint16_t port, int to);
int  sock_recv(int sid, void *buf, int len, int flags, uint32_t *ip, uint16_t *port);
int  sock_shutdown(int sid, int how);             // 0 read, 1 write, 2 both
int  sock_close(int sid);
int  sock_name(int sid, int peer, uint32_t *ip, uint16_t *port);
int  sock_setopt(int sid, int opt, int val);
int  sock_getopt(int sid, int opt);
// Wait until one of them is ready, or timeout_ms (-1 forever, 0 just look).
// Fills revents; returns how many are ready.
int  sock_poll(struct sock_pollent *p, int n, int timeout_ms);

// The program `pid` has ended: its sockets go with it.
void sock_release_owner(int pid);

#define SKF_PEEK     0x02
#define SKF_DONTWAIT 0x40

#endif
