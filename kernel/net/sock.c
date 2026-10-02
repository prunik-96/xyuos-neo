// Sockets for programs. See sock.h.

#include "sock.h"
#include "net.h"
#include "nic.h"
#include "../kernel/process.h"
#include "../kernel/signal.h"
#include "../arch/x86_64/pit.h"

#define S_EINTR          4
#define S_EBADF          9
#define S_EAGAIN        11
#define S_EINVAL        22
#define S_EMFILE        24
#define S_EPIPE         32
#define S_ENOTSOCK      88
#define S_EDESTADDRREQ  89
#define S_EPROTONOSUPPORT 93
#define S_EOPNOTSUPP    95
#define S_ENETDOWN     100
#define S_ECONNRESET   104
#define S_ENOBUFS      105
#define S_EISCONN      106
#define S_ENOTCONN     107
#define S_EALREADY     114
#define S_EINPROGRESS  115

#define RX_CAP (64 * 1024)          // all a window can say without scaling

enum { SS_FREE, SS_NEW, SS_LISTEN, SS_CONNECTING, SS_CONNECTED, SS_DEAD };

struct sock {
    int      used, type, owner, state;
    uint32_t lip;  uint16_t lport;  // what it was bound to
    uint32_t pip;  uint16_t pport;  // a datagram socket's default peer
    int      h;                     // the TCP connection
    int      lsn;                   // the TCP listener
    int      u;                     // the UDP port
    int      nonblock, rcvtimeo, sndtimeo;
    int      shut_rd, shut_wr;
    int      err;                   // waiting to be read by SO_ERROR
};
static struct sock sk[SOCK_MAX];

static int owner_now(void) {
    process_t *p = process_current();
    if (!p) return 0;
    return p->is_thread ? p->tgid : p->pid;
}

static struct sock *get(int sid) {
    if (sid < 0 || sid >= SOCK_MAX || !sk[sid].used) return 0;
    if (sk[sid].owner != owner_now()) return 0;
    return &sk[sid];
}

static int is_loop(uint32_t ip) { return (ip >> 24) == 127; }

// The network, for an address that needs it: brought up if it is not.
static int need_net(uint32_t ip) {
    if (is_loop(ip) || net_is_up()) return 0;
    return net_up() ? 0 : -S_ENETDOWN;
}

// A connection under way: has it finished, one way or the other?
static void refresh(struct sock *s) {
    if (s->type != SK_STREAM) return;
    if (s->state == SS_CONNECTING) {
        int st = net_tcp_state(s->h);
        if (st == NET_TCP_OPEN) s->state = SS_CONNECTED;
        else if (st == NET_TCP_DEAD) {
            s->err = net_tcp_error(s->h);
            if (!s->err) s->err = S_ECONNRESET;
            s->state = SS_DEAD;
        }
    } else if (s->state == SS_CONNECTED && net_tcp_state(s->h) == NET_TCP_DEAD &&
               net_tcp_error(s->h) && !s->err) {
        s->err = net_tcp_error(s->h);
    }
}

// --- readiness ------------------------------------------------------------------

static short revents_of(struct sock *s) {
    refresh(s);
    short r = 0;
    if (s->type == SK_DGRAM) {
        if (s->u >= 0 && net_udp_pending(s->u)) r |= SKP_IN;
        r |= SKP_OUT;
        return r;
    }
    switch (s->state) {
    case SS_LISTEN:
        if (net_tcp_pending(s->lsn)) r |= SKP_IN;
        break;
    case SS_CONNECTING:
        break;
    case SS_CONNECTED:
        if (net_tcp_avail(s->h) || net_tcp_closed(s->h) || s->shut_rd) r |= SKP_IN;
        if (net_tcp_state(s->h) == NET_TCP_DEAD) r |= SKP_HUP | (s->err ? SKP_ERR : 0);
        else if (!s->shut_wr && net_tcp_send_room(s->h) > 0) r |= SKP_OUT;
        break;
    case SS_DEAD:
        r |= SKP_IN | SKP_OUT | SKP_HUP | SKP_ERR;
        break;
    default:                                    // never connected
        r |= SKP_OUT | SKP_HUP;
        break;
    }
    return r;
}

static int ready_in(void *x)  { return (revents_of(x) & (SKP_IN | SKP_ERR | SKP_HUP)) != 0; }
static int ready_out(void *x) { return (revents_of(x) & (SKP_OUT | SKP_ERR | SKP_HUP)) != 0; }
static int ready_conn(void *x) { struct sock *s = x; refresh(s); return s->state != SS_CONNECTING; }

// Look hard for a moment -- on the loopback or a LAN the answer is often
// already on its way -- and then sleep a tick, so the rest of the machine
// runs while this waits. 1 ready, -EAGAIN at the timeout, -EINTR for a
// signal with a handler (one that ends the program never comes back here).
// `s`, when given, is the socket waited on: closed meanwhile (by another
// thread of the program) is -EBADF.
static int wait_until(int (*ready)(void *), void *ctx, struct sock *s, int timeout_ms) {
    uint64_t start = pit_now_us();
    for (;;) {
        uint64_t spin_end = pit_now_us() + 300;
        do {
            net_poll();
            if (s && !s->used) return -S_EBADF;
            if (ready(ctx)) return 1;
        } while (pit_now_us() < spin_end);
        if (timeout_ms == 0) return -S_EAGAIN;
        if (timeout_ms > 0 && pit_now_us() - start >= (uint64_t)timeout_ms * 1000) return -S_EAGAIN;
        process_sleep_ms(10);
        if (signal_deliverable(process_current())) return -S_EINTR;
    }
}

static int wait_for(struct sock *s, int (*ready)(void *), int timeout_ms) {
    return wait_until(ready, s, s, timeout_ms);
}

// --- the calls --------------------------------------------------------------------

int sock_open(int type) {
    if (type != SK_STREAM && type != SK_DGRAM) return -S_EPROTONOSUPPORT;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (sk[i].used) continue;
        struct sock *s = &sk[i];
        for (unsigned k = 0; k < sizeof *s; k++) ((char *)s)[k] = 0;
        s->used = 1;
        s->type = type;
        s->owner = owner_now();
        s->state = SS_NEW;
        s->h = s->lsn = s->u = -1;
        return i;
    }
    return -S_EMFILE;
}

int sock_bind(int sid, uint32_t ip, uint16_t port) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    if (s->state != SS_NEW || s->lport || s->u >= 0) return -S_EINVAL;
    if (s->type == SK_DGRAM) {
        int u = net_udp_open(ip, port);
        if (u < 0) return u;
        s->u = u;
        s->lip = ip;
        s->lport = net_udp_port(u);
        return 0;
    }
    s->lip = ip;
    s->lport = port;
    return 0;
}

int sock_listen(int sid, int backlog) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    if (s->type != SK_STREAM) return -S_EOPNOTSUPP;
    if (s->state == SS_LISTEN) return 0;
    if (s->state != SS_NEW || !s->lport) return -S_EINVAL;
    // Listening on every address means wanting to be reached from outside:
    // the network up, if there is a cable in to bring it up on.
    if (!s->lip && !net_is_up() && nic_init() && nic_link()) net_up();
    int L = net_tcp_listen(s->lip, s->lport, backlog, RX_CAP);
    if (L < 0) return L;
    s->lsn = L;
    s->state = SS_LISTEN;
    return 0;
}

int sock_accept(int sid, uint32_t *ip, uint16_t *port) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    if (s->state != SS_LISTEN) return -S_EINVAL;
    if (!net_tcp_pending(s->lsn)) {
        int w = wait_for(s, ready_in, s->nonblock ? 0 : (s->rcvtimeo ? s->rcvtimeo : -1));
        if (w < 0) return w;
    }
    int h = net_tcp_accept(s->lsn);
    if (h < 0) return -S_EAGAIN;
    int n = sock_open(SK_STREAM);
    if (n < 0) { net_tcp_abort(h); return n; }
    struct sock *c = &sk[n];
    c->h = h;
    c->state = SS_CONNECTED;
    net_tcp_names(h, &c->lip, &c->lport, ip, port);
    return n;
}

int sock_connect(int sid, uint32_t ip, uint16_t port) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    if (s->type == SK_DGRAM) {
        // A default peer, nothing more; bound now so it has a port to answer to.
        int e = need_net(ip);
        if (e) return e;
        if (s->u < 0) {
            int u = net_udp_open(0, 0);
            if (u < 0) return u;
            s->u = u;
            s->lport = net_udp_port(u);
        }
        s->pip = ip;
        s->pport = port;
        return 0;
    }
    refresh(s);
    if (s->state == SS_CONNECTED) return s->nonblock ? -S_EISCONN : 0;
    if (s->state == SS_CONNECTING) {
        if (s->nonblock) return -S_EALREADY;
    } else if (s->state == SS_NEW) {
        if (!port) return -S_EINVAL;
        int e = need_net(ip);
        if (e) return e;
        int h = net_tcp_start(ip, port, RX_CAP);
        if (h < 0) return -S_ENOBUFS;
        s->h = h;
        s->state = SS_CONNECTING;
        if (s->nonblock) return -S_EINPROGRESS;
    } else {
        return -S_EINVAL;
    }
    // The handshake gives up by itself (five SYNs, three seconds).
    int w = wait_for(s, ready_conn, -1);
    if (w < 0) return w;
    if (s->state == SS_CONNECTED) return 0;
    int err = s->err ? s->err : S_ECONNRESET;
    s->err = 0;
    return -err;
}

int sock_send(int sid, const void *buf, int len, int flags, uint32_t ip, uint16_t port, int to) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    if (len < 0) return -S_EINVAL;
    int dontwait = s->nonblock || (flags & SKF_DONTWAIT);

    if (s->type == SK_DGRAM) {
        if (!to) {
            if (!s->pport) return -S_EDESTADDRREQ;
            ip = s->pip;
            port = s->pport;
        }
        int e = need_net(ip);
        if (e) return e;
        if (s->u < 0) {
            int u = net_udp_open(0, 0);
            if (u < 0) return u;
            s->u = u;
            s->lport = net_udp_port(u);
        }
        net_poll();
        return net_udp_send(s->u, ip, port, buf, len);
    }

    refresh(s);
    if (s->state == SS_CONNECTING && !dontwait) {
        int w = wait_for(s, ready_conn, -1);
        if (w < 0) return w;
    }
    if (s->state == SS_DEAD || s->shut_wr) return -S_EPIPE;
    if (s->state != SS_CONNECTED) return s->state == SS_CONNECTING ? -S_EAGAIN : -S_ENOTCONN;

    const uint8_t *p = buf;
    int done = 0;
    while (done < len) {
        int n = net_tcp_send_some(s->h, p + done, len - done);
        if (n < 0) {
            refresh(s);
            return done ? done : -(net_tcp_error(s->h) ? net_tcp_error(s->h) : S_EPIPE);
        }
        done += n;
        if (done == len) break;
        if (dontwait) return done ? done : -S_EAGAIN;
        int w = wait_for(s, ready_out, s->sndtimeo ? s->sndtimeo : -1);
        if (w < 0) return done ? done : w;
    }
    net_poll();
    return done;
}

int sock_recv(int sid, void *buf, int len, int flags, uint32_t *ip, uint16_t *port) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    if (len < 0) return -S_EINVAL;
    int dontwait = s->nonblock || (flags & SKF_DONTWAIT);
    int peek = flags & SKF_PEEK;
    int tmo = dontwait ? 0 : (s->rcvtimeo ? s->rcvtimeo : -1);

    if (s->type == SK_DGRAM) {
        if (s->u < 0) {
            int u = net_udp_open(0, 0);
            if (u < 0) return u;
            s->u = u;
            s->lport = net_udp_port(u);
        }
        net_poll();
        if (!net_udp_pending(s->u)) {
            int w = wait_for(s, ready_in, tmo);
            if (w < 0) return w;
        }
        int whole = net_udp_recv(s->u, buf, len, ip, port, peek);
        if (whole < 0) return -S_EAGAIN;
        return whole < len ? whole : len;
    }

    refresh(s);
    if (s->state == SS_CONNECTING) {
        if (dontwait) return -S_EAGAIN;
        int w = wait_for(s, ready_conn, -1);
        if (w < 0) return w;
    }
    if (s->state == SS_DEAD) { int e = s->err; s->err = 0; return e ? -e : 0; }
    if (s->state != SS_CONNECTED) return -S_ENOTCONN;
    if (s->shut_rd) return 0;

    net_poll();
    if (!net_tcp_avail(s->h) && !net_tcp_closed(s->h)) {
        int w = wait_for(s, ready_in, tmo);
        if (w < 0) return w;
    }
    int avail = net_tcp_avail(s->h);
    if (avail > 0) {
        int n = avail < len ? avail : len;
        const uint8_t *src = net_tcp_peek(s->h);
        uint8_t *dst = buf;
        for (int i = 0; i < n; i++) dst[i] = src[i];
        if (!peek) net_tcp_consume(s->h, n);
        if (ip || port) net_tcp_names(s->h, 0, 0, ip, port);
        return n;
    }
    // Nothing, and nothing more coming: the end, or the reason it ended.
    int e = net_tcp_error(s->h);
    if (e == NET_ECONNRESET && !s->err) return -S_ECONNRESET;
    return 0;
}

int sock_shutdown(int sid, int how) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    if (how < 0 || how > 2) return -S_EINVAL;
    if (s->type == SK_STREAM && s->state != SS_CONNECTED) return -S_ENOTCONN;
    if (how == 0 || how == 2) s->shut_rd = 1;
    if ((how == 1 || how == 2) && !s->shut_wr) {
        s->shut_wr = 1;
        if (s->type == SK_STREAM) net_tcp_shutdown(s->h);
    }
    return 0;
}

static void sock_free(struct sock *s) {
    if (s->type == SK_STREAM) {
        if (s->state == SS_LISTEN) net_tcp_unlisten(s->lsn);
        else if (s->h >= 0) net_tcp_release(s->h);     // its last bytes and FIN still go
    } else if (s->u >= 0) {
        net_udp_close(s->u);
    }
    s->used = 0;
    s->state = SS_FREE;
}

int sock_close(int sid) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    sock_free(s);
    return 0;
}

void sock_release_owner(int pid) {
    for (int i = 0; i < SOCK_MAX; i++)
        if (sk[i].used && sk[i].owner == pid) sock_free(&sk[i]);
}

int sock_name(int sid, int peer, uint32_t *ip, uint16_t *port) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    refresh(s);
    if (s->type == SK_DGRAM) {
        if (peer) {
            if (!s->pport) return -S_ENOTCONN;
            *ip = s->pip; *port = s->pport;
        } else {
            *ip = s->lip; *port = s->lport;
        }
        return 0;
    }
    if (s->state == SS_CONNECTED || s->state == SS_CONNECTING) {
        if (peer) net_tcp_names(s->h, 0, 0, ip, port);
        else net_tcp_names(s->h, ip, port, 0, 0);
        return 0;
    }
    if (peer) return -S_ENOTCONN;
    *ip = s->lip;
    *port = s->lport;
    return 0;
}

int sock_setopt(int sid, int opt, int val) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    switch (opt) {
    case SKO_NONBLOCK: s->nonblock = val ? 1 : 0; return 0;
    case SKO_RCVTIMEO: s->rcvtimeo = val < 0 ? 0 : val; return 0;
    case SKO_SNDTIMEO: s->sndtimeo = val < 0 ? 0 : val; return 0;
    default: return -S_EINVAL;
    }
}

int sock_getopt(int sid, int opt) {
    struct sock *s = get(sid);
    if (!s) return -S_EBADF;
    switch (opt) {
    case SKO_NONBLOCK: return s->nonblock;
    case SKO_RCVTIMEO: return s->rcvtimeo;
    case SKO_SNDTIMEO: return s->sndtimeo;
    case SKO_TYPE:     return s->type;
    case SKO_ERROR: {
        refresh(s);
        int e = s->err;
        s->err = 0;
        return e;
    }
    case SKO_NREAD:
        net_poll();
        if (s->type == SK_DGRAM) {
            if (s->u < 0 || !net_udp_pending(s->u)) return 0;
            uint8_t none;
            return net_udp_recv(s->u, &none, 0, 0, 0, 1);
        }
        return s->state == SS_CONNECTED ? net_tcp_avail(s->h) : 0;
    default:
        return -S_EINVAL;
    }
}

// --- poll -------------------------------------------------------------------------

// The set travels with the wait (it is the caller's memory, and only ever
// looked at while the caller is the one running).
struct poll_set { struct sock_pollent *p; int n; };

static int poll_scan(struct poll_set *ps) {
    int ready = 0;
    for (int i = 0; i < ps->n; i++) {
        struct sock_pollent *e = &ps->p[i];
        if (e->sid < 0) { e->revents = 0; continue; }
        struct sock *s = get(e->sid);
        short r = s ? revents_of(s) : SKP_NVAL;
        e->revents = (short)(r & (e->events | SKP_ERR | SKP_HUP | SKP_NVAL));
        if (e->revents) ready++;
    }
    return ready;
}

static int poll_ready(void *x) { return poll_scan(x) > 0; }

int sock_poll(struct sock_pollent *p, int n, int timeout_ms) {
    if (n < 0 || n > 64) return -S_EINVAL;
    struct poll_set ps = { p, n };
    net_poll();
    int r = poll_scan(&ps);
    if (r || timeout_ms == 0) return r;
    int w = wait_until(poll_ready, &ps, 0, timeout_ms);
    if (w == -S_EINTR) return w;
    return poll_scan(&ps);
}
