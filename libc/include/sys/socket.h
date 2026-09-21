/* The shapes an address comes in.
 *
 * Deliberately not a socket API. This system reaches the network through one
 * kernel call, not through file descriptors, and there is no honest way to
 * offer socket() here. Programs that want addresses -- to tell a hostname
 * from a dotted quad, say -- want only what is below, and it all works.
 */
#ifndef SYS_SOCKET_H
#define SYS_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef unsigned int   socklen_t;
typedef unsigned short sa_family_t;

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10

#define PF_UNSPEC AF_UNSPEC
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

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

#endif
