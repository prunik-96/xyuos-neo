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

// --- TCP as a byte stream (used by the TLS layer) ------------------------
// One connection at a time, the same one net_http_get uses. open/write/close
// are what they look like; fill waits for at least `want` bytes to have
// arrived (or the timeout, or the peer closing) and returns how many are
// there, peek looks at them without removing them, and consume drops the
// leading n once they have been used.
int  net_tcp_open(uint32_t ip, uint16_t port);
int  net_tcp_write(const void *data, int len);
int  net_tcp_fill(int want, uint32_t timeout_ms);
int  net_tcp_avail(void);
const uint8_t *net_tcp_peek(void);
void net_tcp_consume(int n);
int  net_tcp_closed(void);
void net_tcp_shutdown(void);

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
// One at a time, because there is one TCP connection.

#define NET_FETCH_PENDING (-2)

// Begin a fetch. Returns 1 if it was accepted, 0 if one is already running.
// `body` is a form being posted, or 0 for an ordinary GET.
int net_fetch_start(const char *host, const char *path, uint16_t port,
                    int tls, int keep_headers, const char *body, int blen,
                    const char *xhdr, int xhdrlen);

// Let it run for a few milliseconds. Returns NET_FETCH_PENDING while it is
// still going, otherwise the byte count (or -1). *progress, if given, is how
// many bytes have arrived so far.
int net_fetch_poll(int *progress);

// Copy the finished result out and free the slot. Returns the byte count.
int net_fetch_take(char *dst, int max);

// True while a fetch is in flight.
int net_fetch_busy(void);

#endif
