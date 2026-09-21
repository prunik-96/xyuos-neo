/* Internet addresses -- the numbers, not the sockets.
 *
 * A browser needs these to read an address out of a URL and decide whether it
 * is a name or a number: "http://192.168.1.1/" has to be told from
 * "http://example.com/" before anything is looked up. That is arithmetic on
 * text, and it works here.
 *
 * What is NOT here is a socket. This system does its networking in the kernel
 * behind one call, and declaring socket() and connect() so that programs
 * compile and then fail would be worse than not declaring them.
 */
#ifndef NETINET_IN_H
#define NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;             /* network byte order */
};

struct in6_addr {
    union {
        uint8_t  __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
};
#define s6_addr   __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

/* This is a little-endian machine, so network order is the other way round. */
static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint16_t ntohs(uint16_t v) { return htons(v); }

static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#endif
