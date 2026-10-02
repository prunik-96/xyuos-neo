#ifndef NET_H
#define NET_H

#include <stdint.h>

// A small IPv4 stack: Ethernet / ARP / IP / ICMP / UDP / DHCP / DNS / TCP, and
// just enough HTTP to fetch a URL. Client-initiated and poll-driven -- there is
// no interrupt or background thread; every blocking call pumps the RX ring
// itself while it waits (see net_poll).

// Bring the network up: initialise the NIC and lease an address over DHCP.
// Returns 1 on success. Idempotent -- a second call just reports the state.
int net_up(void);

// 1 once net_up() has succeeded and we hold a DHCP lease.
int net_is_up(void);

// Current configuration (host byte order). Any pointer may be NULL.
void net_config(uint32_t *ip, uint32_t *gw, uint32_t *mask, uint32_t *dns);

// The NIC MAC (6 bytes), or NULL if there is no card.
const uint8_t *net_mac(void);

// Feed one received Ethernet frame to the stack. Called by the NIC driver.
void net_rx(const uint8_t *frame, uint16_t len);

// Pump the NIC once (drains its RX ring into net_rx). Blocking helpers call
// this in their wait loops; nothing else needs to.
void net_poll(void);

// --- operations exposed to userland via the SYS_NET syscall ---------------

// One ICMP echo to `ip` (host order). Returns 1 and fills *rtt_us on reply,
// 0 on timeout. Microseconds, because a reply from the next machine along
// arrives well inside a millisecond and "0 ms" tells nobody anything.
int net_ping(uint32_t ip, uint32_t *rtt_us);

// Resolve a hostname to an IPv4 address (host order) via the DHCP-learnt DNS
// server. Returns 1 on success.
int net_resolve(const char *name, uint32_t *ip);

// HTTP/1.0 GET of http://host<:port>/path. `host` is sent in the Host: header;
// `ip` (host order) is where we actually connect. Writes up to `max` bytes of
// the RESPONSE BODY into buf and returns the byte count, or -1 on error.
// `keep_headers` returns the response as it arrived -- status line, headers,
// blank line, body -- instead of the body alone.
// `body` is a form being posted, or 0 for an ordinary GET.
// `xhdr` is extra request headers, each already ending in CRLF, or 0.
int net_http_get(const char *host, uint32_t ip, uint16_t port,
                 const char *path, char *buf, int max, int keep_headers,
                 const char *body, int blen, const char *xhdr);

// --- TCP as a byte stream ------------------------------------------------
//
// Up to eight connections at once. Every call names one by the handle that
// open returned, and there is no current connection and no default -- an
// implicit one is exactly how this came to be single-connection before.
//
// open/write are what they look like; fill waits for at least `want` bytes
// to have arrived (or the timeout, or the peer closing) and returns how many
// are there, peek looks at them without removing them, and consume drops the
// leading n once they have been used. `rx_cap` is how much of the stream may
// sit unread before the peer is told to stop: a path that accumulates a whole
// response wants a great deal, one that drains as it goes wants very little.
//
// shutdown sends the FIN but leaves what already arrived readable; release
// gives the handle back and frees the buffer, and must be called or the
// eight run out.
int  net_tcp_open(uint32_t ip, uint16_t port, int rx_cap);
int  net_tcp_write(int h, const void *data, int len);
int  net_tcp_fill(int h, int want, uint32_t timeout_ms);
int  net_tcp_avail(int h);
const uint8_t *net_tcp_peek(int h);
void net_tcp_consume(int h, int n);
int  net_tcp_closed(int h);
void net_tcp_shutdown(int h);
void net_tcp_release(int h);

// How many of the eight are currently held. For the status line and tests.
int  net_tcp_open_count(void);

// --- for programs' sockets ---------------------------------------------------
//
// Error numbers are the usual ones (the same as libc's errno), returned
// negated where a call returns a count.
#define NET_EINVAL        22
#define NET_EMSGSIZE      90
#define NET_EADDRINUSE    98
#define NET_ENETDOWN     100
#define NET_ECONNRESET   104
#define NET_ENOBUFS      105
#define NET_ETIMEDOUT    110
#define NET_ECONNREFUSED 111
#define NET_EHOSTUNREACH 113

#define NET_TCP_DEAD    0        // reset, timed out, or never was
#define NET_TCP_OPENING 1        // handshake under way
#define NET_TCP_OPEN    2        // data can flow (the peer may have closed its side)

int  net_tcp_start(uint32_t ip, uint16_t port, int rx_cap);   // no waiting: SYN sent
int  net_tcp_state(int h);
int  net_tcp_error(int h);                 // why it is dead: NET_E*, or 0
int  net_tcp_send_some(int h, const void *data, int len);     // queued now, or -1
int  net_tcp_send_room(int h);
int  net_tcp_unacked(int h);
void net_tcp_abort(int h);                 // a reset, and the handle back at once
void net_tcp_names(int h, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport);

// A port to accept connections on (ip 0: any address of ours). Returns a
// listener, or -NET_E*. accept gives the next finished handshake's handle,
// or -1 when none is waiting.
int  net_tcp_listen(uint32_t ip, uint16_t port, int backlog, int rx_cap);
int  net_tcp_pending(int listener);
int  net_tcp_accept(int listener);
void net_tcp_unlisten(int listener);

// UDP: a port with a queue of datagrams. open returns the socket or -NET_E*.
int      net_udp_open(uint32_t ip, uint16_t port);       // port 0: any free one
uint16_t net_udp_port(int u);
int      net_udp_send(int u, uint32_t ip, uint16_t port, const void *data, int len);
int      net_udp_recv(int u, void *buf, int len, uint32_t *ip, uint16_t *port, int peek);
int      net_udp_pending(int u);
void     net_udp_close(int u);

int  net_active(void);       // anything open that needs looking after
void net_service(void);      // look after it (the scheduler calls this)

// One NTP exchange with `ip` (the simple client of RFC 4330), up to `tries`
// requests a second apart. On an answer: *utc_ms is UTC at the moment it
// arrived, *at_us that moment by pit_now_us(), *rtt_ms the round trip less the
// server's own time. Returns 1 then, else 0.
int net_ntp(uint32_t ip, int tries, int64_t *utc_ms, uint64_t *at_us, uint32_t *rtt_ms);

// Parse "a.b.c.d" into a host-order address. Returns 1 on success.
int net_parse_ip(const char *s, uint32_t *out);

// --- the request log -----------------------------------------------------
//
// One line per fetch: where it went, how long it took, how much came back.
// A ring, written by whoever performed the request and read out whole by
// userland. This exists because the alternative was kprintf, and a kernel
// that prints draws over the window the user is looking at.

#define NET_LOG_SLOTS   64
#define NET_LOG_TLS     1        // https rather than http
#define NET_LOG_REUSED  2        // no handshake: an open connection was there
#define NET_LOG_FAIL    4        // it did not come back
#define NET_LOG_CACHED  8        // never reached the network at all

struct net_log_ent {
    uint32_t start_ms;           // when it began, since boot
    uint32_t dur_ms;             // how long it took
    int32_t  bytes;              // body bytes, or -1
    int16_t  status;             // HTTP status, 0 if we never got one
    uint8_t  flags;
    uint8_t  pad;
    char     host[64];
    char     path[144];
};

void net_log_add(const char *host, const char *path, int bytes, int status,
                 uint32_t start_ms, unsigned flags);

// Copies the entries oldest-first into `out` and returns how many. `max` is
// how many fit.
int  net_log_read(struct net_log_ent *out, int max);
void net_log_clear(void);

// Milliseconds since boot -- the same clock the log is stamped with.
uint32_t net_ms(void);

// --- fetching without stopping the machine -------------------------------
//
// A fetch is seconds of waiting, and it used to all happen inside one system
// call: interrupts off, no scheduler, nothing on screen moving, for as long as
// the far end took. The work itself is unchanged -- it runs on its own stack
// now and hands the processor back every few milliseconds, and the caller asks
// again in a moment. Everything else on the machine runs in between.
//
// Four at a time, each on its own TCP connection and its own TLS session.
// Every call names the one it means by the slot that start returned.

#define NET_FETCH_PENDING (-2)

// Begin a fetch. Returns the slot it was given, or -1 if all of them are
// busy. `body` is a form being posted, or 0 for an ordinary GET.
int net_fetch_start(const char *host, const char *path, uint16_t port,
                    int tls, int keep_headers, const char *body, int blen,
                    const char *xhdr, int xhdrlen);

// Let that slot run for a few milliseconds. Returns NET_FETCH_PENDING while
// it is still going, otherwise the byte count (or -1). *progress, if given,
// is how many bytes have arrived so far.
int net_fetch_poll(int slot, int *progress);

// Copy the finished result out and free the slot. Returns the byte count.
int net_fetch_take(int slot, char *dst, int max);

// True while any fetch is in flight.
int net_fetch_busy(void);

// How many slots there are.
int net_fetch_slots(void);

// Give up on a fetch and free its slot. MUST be called for every slot a
// program still holds before it exits: a fetch left suspended has a saved
// return into that program's kernel stack, and when the stack is freed the
// next switch onto it takes the machine down with no way to report it.
void net_fetch_cancel(int slot);

#endif
