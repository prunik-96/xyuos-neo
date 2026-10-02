/* Sockets (SYS_SOCKET), names to addresses, and waiting on both.
 *
 * A socket's descriptor is SOCK_FD_BASE plus the kernel's number for it:
 * files are numbered from 3 and there are sixteen of them at most, so the
 * two never meet, and both fit in an fd_set. */

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xyuos_syscall.h"

#define SOCK_FD_BASE 32
#define SOCK_N       32

int __sock_fd(int fd) { return fd >= SOCK_FD_BASE && fd < SOCK_FD_BASE + SOCK_N; }
#define SID(fd) ((fd) - SOCK_FD_BASE)

static int call(struct sock_req *r) {
    xyuos_syscall3(SYS_SOCKET, (long)r, 0, 0);
    if (r->result < 0) { errno = -r->result; return -1; }
    return r->result;
}

static int check(int fd) {
    if (!__sock_fd(fd)) { errno = (fd >= 0 && fd < SOCK_FD_BASE) ? ENOTSOCK : EBADF; return 0; }
    return 1;
}

static int setopt(int fd, int opt, int val) {
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_SETOPT; r.sid = SID(fd); r.arg = opt; r.val = val;
    return call(&r);
}

static int getopt_k(int fd, int opt) {
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_GETOPT; r.sid = SID(fd); r.arg = opt;
    return call(&r);
}

/* The kernel's option numbers (kernel/net/sock.h). */
#define SKO_NONBLOCK 1
#define SKO_RCVTIMEO 2
#define SKO_SNDTIMEO 3
#define SKO_ERROR    4
#define SKO_TYPE     5
#define SKO_NREAD    6

static int addr_in(const struct sockaddr *a, socklen_t len, unsigned *ip, unsigned short *port) {
    if (!a || len < sizeof(struct sockaddr_in)) { errno = EINVAL; return -1; }
    if (a->sa_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    const struct sockaddr_in *s = (const struct sockaddr_in *)a;
    *ip = ntohl(s->sin_addr.s_addr);
    *port = ntohs(s->sin_port);
    return 0;
}

static void addr_out(struct sockaddr *a, socklen_t *len, unsigned ip, unsigned short port) {
    if (!a || !len) return;
    struct sockaddr_in s;
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    s.sin_port = htons(port);
    s.sin_addr.s_addr = htonl(ip);
    socklen_t n = *len < sizeof s ? *len : (socklen_t)sizeof s;
    memcpy(a, &s, n);
    *len = sizeof s;
}

/* --- the calls ---------------------------------------------------------------- */

int socket(int domain, int type, int protocol) {
    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    int nb = type & SOCK_NONBLOCK;
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (type != SOCK_STREAM && type != SOCK_DGRAM) { errno = EPROTONOSUPPORT; return -1; }
    if (protocol && !(type == SOCK_STREAM && protocol == IPPROTO_TCP) &&
        !(type == SOCK_DGRAM && protocol == IPPROTO_UDP)) { errno = EPROTONOSUPPORT; return -1; }
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_OPEN;
    r.arg = type;
    int sid = call(&r);
    if (sid < 0) return -1;
    int fd = SOCK_FD_BASE + sid;
    if (nb) setopt(fd, SKO_NONBLOCK, 1);
    return fd;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    if (!check(fd)) return -1;
    struct sock_req r;
    memset(&r, 0, sizeof r);
    if (addr_in(addr, len, &r.ip, &r.port) < 0) return -1;
    r.op = SOCKOP_BIND; r.sid = SID(fd);
    return call(&r) < 0 ? -1 : 0;
}

int listen(int fd, int backlog) {
    if (!check(fd)) return -1;
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_LISTEN; r.sid = SID(fd); r.arg = backlog > 0 ? backlog : SOMAXCONN;
    return call(&r) < 0 ? -1 : 0;
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags) {
    if (!check(fd)) return -1;
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_ACCEPT; r.sid = SID(fd);
    int sid = call(&r);
    if (sid < 0) return -1;
    addr_out(addr, len, r.ip, r.port);
    int nfd = SOCK_FD_BASE + sid;
    if (flags & SOCK_NONBLOCK) setopt(nfd, SKO_NONBLOCK, 1);
    return nfd;
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) { return accept4(fd, addr, len, 0); }

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if (!check(fd)) return -1;
    struct sock_req r;
    memset(&r, 0, sizeof r);
    if (addr_in(addr, len, &r.ip, &r.port) < 0) return -1;
    r.op = SOCKOP_CONNECT; r.sid = SID(fd);
    return call(&r) < 0 ? -1 : 0;
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen) {
    if (!check(fd)) return -1;
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_SEND;
    if (to) {
        if (addr_in(to, tolen, &r.ip, &r.port) < 0) return -1;
        r.op = SOCKOP_SENDTO;
    }
    r.sid = SID(fd); r.buf = (void *)buf; r.len = (unsigned)len; r.arg = flags;
    return call(&r);
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
    return sendto(fd, buf, len, flags, 0, 0);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen) {
    if (!check(fd)) return -1;
    struct sock_req r;
    size_t got = 0;
    /* MSG_WAITALL: keep at it until all of it has come, or the end. */
    do {
        memset(&r, 0, sizeof r);
        r.op = SOCKOP_RECV; r.sid = SID(fd);
        r.buf = (char *)buf + got; r.len = (unsigned)(len - got);
        r.arg = flags & (MSG_PEEK | MSG_DONTWAIT);
        int n = call(&r);
        if (n < 0) return got ? (ssize_t)got : -1;
        if (n == 0) break;
        got += (size_t)n;
    } while ((flags & MSG_WAITALL) && !(flags & MSG_PEEK) && got < len);
    addr_out(from, fromlen, r.ip, r.port);
    return (ssize_t)got;
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    return recvfrom(fd, buf, len, flags, 0, 0);
}

int shutdown(int fd, int how) {
    if (!check(fd)) return -1;
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_SHUTDOWN; r.sid = SID(fd); r.arg = how;
    return call(&r) < 0 ? -1 : 0;
}

static int name_of(int fd, struct sockaddr *addr, socklen_t *len, int peer) {
    if (!check(fd)) return -1;
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_NAME; r.sid = SID(fd); r.arg = peer;
    if (call(&r) < 0) return -1;
    addr_out(addr, len, r.ip, r.port);
    return 0;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) { return name_of(fd, addr, len, 0); }
int getpeername(int fd, struct sockaddr *addr, socklen_t *len) { return name_of(fd, addr, len, 1); }

static int tv_ms(const void *val, socklen_t len) {
    if (len < sizeof(struct timeval)) return 0;
    const struct timeval *tv = val;
    long ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
    if (ms <= 0 && (tv->tv_sec || tv->tv_usec)) ms = 1;
    return (int)ms;
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len) {
    if (!check(fd)) return -1;
    if (level == SOL_SOCKET && name == SO_RCVTIMEO) return setopt(fd, SKO_RCVTIMEO, tv_ms(val, len)) < 0 ? -1 : 0;
    if (level == SOL_SOCKET && name == SO_SNDTIMEO) return setopt(fd, SKO_SNDTIMEO, tv_ms(val, len)) < 0 ? -1 : 0;
    /* The rest change nothing here, and saying so would only make ported
     * programs give up on something that works. */
    return 0;
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len) {
    if (!check(fd)) return -1;
    if (!val || !len) { errno = EFAULT; return -1; }
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
        int ms = getopt_k(fd, name == SO_RCVTIMEO ? SKO_RCVTIMEO : SKO_SNDTIMEO);
        if (ms < 0 || *len < sizeof(struct timeval)) { errno = EINVAL; return -1; }
        struct timeval *tv = val;
        tv->tv_sec = ms / 1000;
        tv->tv_usec = (ms % 1000) * 1000;
        *len = sizeof(struct timeval);
        return 0;
    }
    int v = 0;
    if (level == SOL_SOCKET && name == SO_ERROR) v = getopt_k(fd, SKO_ERROR);
    else if (level == SOL_SOCKET && name == SO_TYPE) v = getopt_k(fd, SKO_TYPE);
    else if (level == SOL_SOCKET && (name == SO_RCVBUF || name == SO_SNDBUF)) v = 65536;
    if (v < 0) return -1;
    if (*len < sizeof(int)) { errno = EINVAL; return -1; }
    *(int *)val = v;
    *len = sizeof(int);
    return 0;
}

/* --- as descriptors ------------------------------------------------------------- */

long __sock_read(int fd, void *buf, unsigned long len) { return recv(fd, buf, len, 0); }
long __sock_write(int fd, const void *buf, unsigned long len) { return send(fd, buf, len, 0); }

int __sock_close(int fd) {
    struct sock_req r;
    memset(&r, 0, sizeof r);
    r.op = SOCKOP_CLOSE; r.sid = SID(fd);
    return call(&r) < 0 ? -1 : 0;
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (!__sock_fd(fd)) return cmd == F_GETFL ? O_RDWR : 0;
    if (cmd == F_GETFL) {
        int nb = getopt_k(fd, SKO_NONBLOCK);
        return nb < 0 ? -1 : (O_RDWR | (nb ? O_NONBLOCK : 0));
    }
    if (cmd == F_SETFL) return setopt(fd, SKO_NONBLOCK, (arg & O_NONBLOCK) ? 1 : 0) < 0 ? -1 : 0;
    return 0;
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    int *p = va_arg(ap, int *);
    va_end(ap);
    if (!check(fd)) return -1;
    if (req == FIONREAD) { int n = getopt_k(fd, SKO_NREAD); if (n < 0) return -1; *p = n; return 0; }
    if (req == FIONBIO) return setopt(fd, SKO_NONBLOCK, *p ? 1 : 0) < 0 ? -1 : 0;
    errno = EINVAL;
    return -1;
}

/* --- waiting ---------------------------------------------------------------------- */

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    struct sock_pollent sp[64];
    int map[64], ns = 0, ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        if (__sock_fd(fds[i].fd) && ns < 64) {
            sp[ns].sid = SID(fds[i].fd);
            sp[ns].events = (short)(fds[i].events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM));
            if (sp[ns].events & POLLRDNORM) sp[ns].events |= POLLIN;
            if (sp[ns].events & POLLWRNORM) sp[ns].events |= POLLOUT;
            sp[ns].revents = 0;
            map[ns++] = (int)i;
        } else {
            /* A file, or the terminal: never waits. */
            fds[i].revents = fds[i].events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
            if (fds[i].revents) ready++;
        }
    }
    if (ns) {
        struct sock_req r;
        memset(&r, 0, sizeof r);
        r.op = SOCKOP_POLL;
        r.buf = sp;
        r.len = (unsigned)ns;
        r.arg = ready ? 0 : timeout;
        if (call(&r) < 0) return -1;
        for (int k = 0; k < ns; k++) {
            short ev = sp[k].revents;
            struct pollfd *p = &fds[map[k]];
            p->revents = (short)(ev & (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL));
            if ((ev & POLLIN) && (p->events & POLLRDNORM)) p->revents |= POLLRDNORM;
            if ((ev & POLLOUT) && (p->events & POLLWRNORM)) p->revents |= POLLWRNORM;
            if (p->revents) ready++;
        }
    } else if (!ready && timeout > 0) {
        sleep_ms((unsigned)timeout);
    }
    return ready;
}

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv) {
    struct pollfd p[FD_SETSIZE];
    int n = 0;
    if (nfds > FD_SETSIZE) nfds = FD_SETSIZE;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (rd && FD_ISSET(fd, rd)) ev |= POLLIN;
        if (wr && FD_ISSET(fd, wr)) ev |= POLLOUT;
        if (ex && FD_ISSET(fd, ex)) ev |= POLLPRI;
        if (!ev) continue;
        p[n].fd = fd; p[n].events = ev; p[n].revents = 0;
        n++;
    }
    int timeout = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    int r = poll(p, (nfds_t)n, timeout);
    if (r < 0) return -1;
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    int count = 0;
    for (int i = 0; i < n; i++) {
        short re = p[i].revents;
        if (rd && (re & (POLLIN | POLLHUP | POLLERR)) && (p[i].events & POLLIN)) { FD_SET(p[i].fd, rd); count++; }
        if (wr && (re & (POLLOUT | POLLERR)) && (p[i].events & POLLOUT)) { FD_SET(p[i].fd, wr); count++; }
        if (ex && (re & POLLPRI) && (p[i].events & POLLPRI)) { FD_SET(p[i].fd, ex); count++; }
    }
    return count;
}

/* --- names -------------------------------------------------------------------------- */

int h_errno;

in_addr_t inet_addr(const char *src) {
    struct in_addr a;
    return inet_aton(src, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr in) {
    static char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in, buf, sizeof buf);
    return buf;
}

/* A name, a dotted quad or "localhost", to an address in host order. */
static int lookup(const char *name, unsigned *ip, int numeric_only) {
    struct in_addr a;
    if (inet_pton(AF_INET, name, &a) == 1) { *ip = ntohl(a.s_addr); return 0; }
    if (!strcasecmp(name, "localhost") || !strcasecmp(name, "localhost.localdomain")) {
        *ip = INADDR_LOOPBACK;
        return 0;
    }
    if (numeric_only) return EAI_NONAME;
    if (!net_up()) return EAI_AGAIN;
    unsigned v = 0;
    if (net_resolve(name, &v) != 0 || !v) return EAI_NONAME;
    *ip = v;
    return 0;
}

static const struct { const char *name; int port; } services[] = {
    { "echo", 7 }, { "ftp", 21 }, { "ssh", 22 }, { "telnet", 23 }, { "smtp", 25 },
    { "domain", 53 }, { "http", 80 }, { "pop3", 110 }, { "ntp", 123 }, { "imap", 143 },
    { "https", 443 }, { "imaps", 993 }, { "pop3s", 995 }, { "irc", 6667 }, { "http-alt", 8080 },
};

static int service_port(const char *s, int numeric_only) {
    if (!s || !*s) return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (!*end && v >= 0 && v <= 65535) return (int)v;
    if (numeric_only) return -1;
    for (unsigned i = 0; i < sizeof services / sizeof services[0]; i++)
        if (!strcmp(services[i].name, s)) return services[i].port;
    return -1;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    int flags = hints ? hints->ai_flags : 0;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    if (!res) return EAI_FAIL;
    *res = 0;
    if (!node && !service) return EAI_NONAME;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM) return EAI_SOCKTYPE;

    int port = service_port(service, flags & AI_NUMERICSERV);
    if (port < 0) return EAI_SERVICE;

    unsigned ip;
    if (node) {
        int e = lookup(node, &ip, flags & AI_NUMERICHOST);
        if (e) return e;
    } else {
        ip = (flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK;
    }

    int types[2], nt = 0;
    if (socktype) types[nt++] = socktype;
    else { types[nt++] = SOCK_STREAM; types[nt++] = SOCK_DGRAM; }

    struct addrinfo *head = 0, **tail = &head;
    for (int i = 0; i < nt; i++) {
        /* One allocation each: the entry, its address, and its name. */
        size_t nl = (flags & AI_CANONNAME) && node ? strlen(node) + 1 : 0;
        struct addrinfo *ai = calloc(1, sizeof *ai + sizeof(struct sockaddr_in) + nl);
        if (!ai) { freeaddrinfo(head); return EAI_MEMORY; }
        struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
        sin->sin_family = AF_INET;
        sin->sin_port = htons((unsigned short)port);
        sin->sin_addr.s_addr = htonl(ip);
        ai->ai_family = AF_INET;
        ai->ai_socktype = types[i];
        ai->ai_protocol = protocol ? protocol : (types[i] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
        ai->ai_addrlen = sizeof *sin;
        ai->ai_addr = (struct sockaddr *)sin;
        if (nl) {
            ai->ai_canonname = (char *)(sin + 1);
            memcpy(ai->ai_canonname, node, nl);
        }
        *tail = ai;
        tail = &ai->ai_next;
    }
    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

const char *gai_strerror(int err) {
    switch (err) {
    case 0:            return "Success";
    case EAI_NONAME:   return "Name or service not known";
    case EAI_AGAIN:    return "Temporary failure in name resolution";
    case EAI_FAIL:     return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:   return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE:  return "Servname not supported for ai_socktype";
    case EAI_MEMORY:   return "Memory allocation failure";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_OVERFLOW: return "Result too large";
    default:           return "Unknown error";
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)flags;
    if (!sa || salen < sizeof(struct sockaddr_in) || sa->sa_family != AF_INET) return EAI_FAMILY;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    if (host && hostlen) {
        /* No reverse lookups: the number is the name. */
        if (!inet_ntop(AF_INET, &sin->sin_addr, host, hostlen)) return EAI_OVERFLOW;
    }
    if (serv && servlen) {
        if (snprintf(serv, servlen, "%u", (unsigned)ntohs(sin->sin_port)) >= (int)servlen) return EAI_OVERFLOW;
    }
    return 0;
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent h;
    static char hname[256];
    static struct in_addr addr;
    static char *list[2], *aliases[1];
    unsigned ip;
    int e = lookup(name, &ip, 0);
    if (e) { h_errno = e == EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND; return 0; }
    snprintf(hname, sizeof hname, "%s", name);
    addr.s_addr = htonl(ip);
    list[0] = (char *)&addr;
    list[1] = 0;
    aliases[0] = 0;
    h.h_name = hname;
    h.h_aliases = aliases;
    h.h_addrtype = AF_INET;
    h.h_length = 4;
    h.h_addr_list = list;
    return &h;
}

struct servent *getservbyname(const char *name, const char *proto) {
    static struct servent s;
    static char sname[32], sproto[8];
    static char *aliases[1];
    int port = service_port(name, 0);
    if (port <= 0) return 0;
    snprintf(sname, sizeof sname, "%s", name);
    snprintf(sproto, sizeof sproto, "%s", proto ? proto : "tcp");
    aliases[0] = 0;
    s.s_name = sname;
    s.s_aliases = aliases;
    s.s_port = htons((unsigned short)port);
    s.s_proto = sproto;
    return &s;
}

const char *hstrerror(int err) {
    switch (err) {
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN:      return "Host name lookup failure";
    case NO_RECOVERY:    return "Unknown server error";
    case NO_DATA:        return "No address associated with name";
    default:             return "Unknown resolver error";
    }
}
