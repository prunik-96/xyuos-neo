/* Sockets: TCP and UDP over IPv4, the Berkeley way.
 *
 * A socket is a descriptor like a file's: read(), write() and close() work
 * on it, and poll()/select() wait on it. It belongs to the program that made
 * it and is closed when the program ends. 127.0.0.1 is this machine; a
 * socket to anywhere else brings the network up if it is not up yet.
 *
 * What is not here: IPv6, raw sockets, Unix-domain sockets, and options
 * beyond the common ones (the rest are accepted and do nothing).
 */
#ifndef SYS_SOCKET_H
#define SYS_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int   socklen_t;
typedef unsigned short sa_family_t;

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10

#define PF_UNSPEC AF_UNSPEC
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000

#define SOL_SOCKET    1
#define SO_DEBUG      1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_DONTROUTE  5
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_OOBINLINE  10
#define SO_LINGER     13
#define SO_REUSEPORT  15
#define SO_RCVLOWAT   18
#define SO_SNDLOWAT   19
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21

#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_TRUNC     0x20
#define MSG_DONTWAIT  0x40
#define MSG_WAITALL   0x100
#define MSG_NOSIGNAL  0x4000

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOMAXCONN 16

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

/* Big enough for any of the address shapes, and aligned for all of them. */
struct sockaddr_storage {
    sa_family_t ss_family;
    char        __pad[126];
    uint64_t    __align;
};

struct linger { int l_onoff; int l_linger; };

struct iovec;

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr *addr, socklen_t len);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr *addr, socklen_t *len);
int     accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int     connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen);
int     shutdown(int fd, int how);
int     getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int     getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int     setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int     getsockopt(int fd, int level, int name, void *val, socklen_t *len);

#ifdef __cplusplus
}
#endif

#endif
