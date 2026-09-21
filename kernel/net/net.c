#include "net.h"
#include "http.h"
#include "nic.h"
#include "tls.h"
#include "../kernel/kio.h"
#include "../kernel/task.h"
#include "../arch/x86_64/pit.h"
#include <stddef.h>

// ==========================================================================
//  small utilities
// ==========================================================================

static void nmemcpy(void *d, const void *s, int n) {
    uint8_t *dst = d; const uint8_t *src = s;
    // Eight bytes at a time while both sides stay aligned; the tails are the
    // only part that goes one byte at a time.
    while (n >= 8 && !(((uintptr_t)dst | (uintptr_t)src) & 7)) {
        *(uint64_t *)dst = *(const uint64_t *)src;
        dst += 8; src += 8; n -= 8;
    }
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

// Forward-overlapping move, used when the receive buffer is compacted. The
// destination is always BELOW the source there, so copying upwards is safe.
static void nmemmove_down(void *d, const void *s, int n) {
    uint8_t *dst = d; const uint8_t *src = s;
    while (n >= 8) { nmemcpy(dst, src, 8); dst += 8; src += 8; n -= 8; }
    for (int i = 0; i < n; i++) dst[i] = src[i];
}
static void nmemset(void *d, int v, int n) {
    uint8_t *dst = d;
    for (int i = 0; i < n; i++) dst[i] = (uint8_t)v;
}
static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t bswap32(uint32_t v) {
    return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}
#define htons(x) bswap16(x)
#define ntohs(x) bswap16(x)
#define htonl(x) bswap32(x)
#define ntohl(x) bswap32(x)

static uint64_t now_ms(void) { return pit_now_us() / 1000; }

// Both clocks come from the timestamp counter (see pit_now_us): the tick
// counter stops inside a system call, which is exactly where every one of the
// waits below runs.
static uint64_t now_us(void) { return pit_now_us(); }

// One's-complement checksum over a byte range, continuable via `seed`.
static uint32_t csum_partial(const void *data, int len, uint32_t seed) {
    const uint8_t *p = data;
    uint32_t sum = seed;
    while (len > 1) { sum += ((uint16_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (uint16_t)p[0] << 8;
    return sum;
}
static uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}
static uint16_t ip_checksum(const void *data, int len) {
    return csum_fold(csum_partial(data, len, 0));
}

// ==========================================================================
//  wire structures (all fields big-endian on the wire)
// ==========================================================================

#define ETH_ARP 0x0806
#define ETH_IP  0x0800
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ip_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint16_t length, checksum;
} udp_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;
} icmp_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  data_off;      // high nibble = header length in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum, urgent;
} tcp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

// ==========================================================================
//  global state
// ==========================================================================

static int      have_nic = 0;
static int      up = 0;
static uint8_t  our_mac[6];
static uint32_t our_ip = 0, gw_ip = 0, net_mask = 0, dns_ip = 0;   // host order
static uint16_t ip_id = 1;
static uint32_t tx_frames = 0, rx_frames = 0;   // diagnostics

static const uint8_t BCAST_MAC[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

// ==========================================================================
//  Ethernet + IP emit
// ==========================================================================

static uint8_t txscratch[1600];

static int eth_send(const uint8_t *dstmac, uint16_t type,
                    const void *payload, int len) {
    eth_hdr_t *e = (eth_hdr_t *)txscratch;
    nmemcpy(e->dst, dstmac, 6);
    nmemcpy(e->src, our_mac, 6);
    e->type = htons(type);
    nmemcpy(txscratch + sizeof(eth_hdr_t), payload, len);
    int r = nic_send(txscratch, (uint16_t)(sizeof(eth_hdr_t) + len));
    if (r == 0) tx_frames++;
    return r;
}

// Build an IP packet (header + payload already staged in `payload`) and send it
// to a known next-hop MAC. srcip/dstip in host order.
static uint8_t ippkt[1600];
static int ip_emit(const uint8_t *dstmac, uint32_t srcip, uint32_t dstip,
                   uint8_t proto, const void *payload, int len) {
    ip_hdr_t *ih = (ip_hdr_t *)ippkt;
    ih->ver_ihl = 0x45;
    ih->tos = 0;
    ih->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + len));
    ih->id = htons(ip_id++);
    ih->flags_frag = 0;
    ih->ttl = 64;
    ih->proto = proto;
    ih->checksum = 0;
    ih->src = htonl(srcip);
    ih->dst = htonl(dstip);
    ih->checksum = htons(ip_checksum(ih, sizeof(ip_hdr_t)));
    nmemcpy(ippkt + sizeof(ip_hdr_t), payload, len);
    return eth_send(dstmac, ETH_IP, ippkt, sizeof(ip_hdr_t) + len);
}

// ==========================================================================
//  ARP
// ==========================================================================

typedef struct { uint32_t ip; uint8_t mac[6]; int valid; } arp_entry_t;
static arp_entry_t arp_cache[8];

static int arp_lookup(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < 8; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            nmemcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    return 0;
}
static void arp_store(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < 8; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            nmemcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    for (int i = 0; i < 8; i++)
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            nmemcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    arp_cache[0].ip = ip;               // evict slot 0 if full
    nmemcpy(arp_cache[0].mac, mac, 6);
}

static void arp_send(uint16_t oper, uint32_t tpa, const uint8_t *tha) {
    arp_pkt_t a;
    a.htype = htons(1);
    a.ptype = htons(ETH_IP);
    a.hlen = 6; a.plen = 4;
    a.oper = htons(oper);
    nmemcpy(a.sha, our_mac, 6);
    uint32_t spa = htonl(our_ip);
    nmemcpy(a.spa, &spa, 4);
    nmemcpy(a.tha, tha, 6);
    uint32_t tpa_n = htonl(tpa);
    nmemcpy(a.tpa, &tpa_n, 4);
    eth_send(oper == 2 ? tha : BCAST_MAC, ETH_ARP, &a, sizeof(a));
}

static int arp_resolve(uint32_t ip, uint8_t *mac) {
    if (arp_lookup(ip, mac)) return 1;
    for (int tries = 0; tries < 4; tries++) {
        arp_send(1, ip, BCAST_MAC);
        uint64_t deadline = now_ms() + 300;
        while (now_ms() < deadline) {
            net_poll();
            if (arp_lookup(ip, mac)) return 1;
        }
    }
    return 0;
}

// Route a destination to its next-hop MAC (host, or gateway if off-subnet).
static int route_mac(uint32_t dstip, uint8_t *mac) {
    uint32_t nexthop = ((dstip & net_mask) == (our_ip & net_mask)) ? dstip : gw_ip;
    return arp_resolve(nexthop, mac);
}

static int ip_send(uint32_t dstip, uint8_t proto, const void *payload, int len) {
    uint8_t mac[6];
    if (!route_mac(dstip, mac)) return -1;
    return ip_emit(mac, our_ip, dstip, proto, payload, len);
}

// ==========================================================================
//  UDP
// ==========================================================================

static uint8_t udpseg[1600];
static int build_udp(uint32_t srcip, uint32_t dstip, uint16_t sport,
                     uint16_t dport, const void *data, int len) {
    udp_hdr_t *u = (udp_hdr_t *)udpseg;
    u->src_port = htons(sport);
    u->dst_port = htons(dport);
    u->length = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    u->checksum = 0;
    nmemcpy(udpseg + sizeof(udp_hdr_t), data, len);
    // pseudo-header checksum
    uint8_t pseudo[12];
    uint32_t s = htonl(srcip), d = htonl(dstip);
    nmemcpy(pseudo, &s, 4); nmemcpy(pseudo + 4, &d, 4);
    pseudo[8] = 0; pseudo[9] = IPPROTO_UDP;
    uint16_t l = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    nmemcpy(pseudo + 10, &l, 2);
    uint32_t sum = csum_partial(pseudo, 12, 0);
    sum = csum_partial(udpseg, sizeof(udp_hdr_t) + len, sum);
    uint16_t c = csum_fold(sum);
    if (c == 0) c = 0xFFFF;
    u->checksum = htons(c);
    return sizeof(udp_hdr_t) + len;
}

// Broadcast/raw UDP (used by DHCP before we have an address or ARP).
static void udp_send_raw(const uint8_t *dstmac, uint32_t srcip, uint32_t dstip,
                         uint16_t sport, uint16_t dport, const void *data, int len) {
    int seglen = build_udp(srcip, dstip, sport, dport, data, len);
    ip_emit(dstmac, srcip, dstip, IPPROTO_UDP, udpseg, seglen);
}
// Routed UDP (used by DNS).
static int udp_send(uint32_t dstip, uint16_t sport, uint16_t dport,
                    const void *data, int len) {
    int seglen = build_udp(our_ip, dstip, sport, dport, data, len);
    return ip_send(dstip, IPPROTO_UDP, udpseg, seglen);
}

// Single-slot capture for the reply a blocking caller is waiting on.
static volatile int  udp_have = 0;
static uint16_t      udp_dport, udp_sport;
static uint32_t      udp_sip;
static uint8_t       udp_data[1500];
static int           udp_dlen;

// ==========================================================================
//  ICMP (ping)
// ==========================================================================

static volatile int icmp_have = 0;
static uint16_t     icmp_r_id, icmp_r_seq;
static uint32_t     icmp_r_from;

int net_ping(uint32_t ip, uint32_t *rtt_us) {
    static uint16_t seq = 0;
    uint8_t pkt[8 + 32];
    icmp_hdr_t *ic = (icmp_hdr_t *)pkt;
    ic->type = 8; ic->code = 0; ic->checksum = 0;
    ic->id = htons(0x1234);
    ic->seq = htons(++seq);
    for (int i = 0; i < 32; i++) pkt[8 + i] = (uint8_t)i;
    ic->checksum = htons(ip_checksum(pkt, sizeof(pkt)));

    icmp_have = 0;
    uint64_t start = now_ms(), start_us = now_us();
    if (ip_send(ip, IPPROTO_ICMP, pkt, sizeof(pkt)) != 0) return 0;

    uint64_t deadline = start + 1500;
    while (now_ms() < deadline) {
        net_poll();
        if (icmp_have && icmp_r_from == ip && icmp_r_seq == seq) {
            if (rtt_us) *rtt_us = (uint32_t)(now_us() - start_us);
            return 1;
        }
    }
    return 0;
}

// ==========================================================================
//  TCP  (one connection at a time -- enough for a single HTTP fetch)
// ==========================================================================

enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_CLOSING };

static struct {
    int      state;
    uint32_t remote_ip;
    uint16_t remote_port, local_port;
    uint32_t snd_nxt;      // next sequence we will send
    uint32_t rcv_nxt;      // next sequence we expect
    uint8_t  rx[262144];  // a full TLS record must fit, and so must a whole
                          // plain-HTTP response: net_http_get accumulates it
    int      rx_head;     // where the unread bytes start
    int      rx_len;      // how many there are
    int      remote_closed;
} tcp;

// Free space behind the unread bytes, which is what the peer is allowed to
// send us next.
static int tcp_rx_space(void) {
    return (int)sizeof(tcp.rx) - tcp.rx_head - tcp.rx_len;
}

static void tcp_out(uint8_t flags, const void *data, int len) {
    uint8_t seg[sizeof(tcp_hdr_t) + 1460];
    if (len > 1460) len = 1460;
    tcp_hdr_t *t = (tcp_hdr_t *)seg;
    t->src_port = htons(tcp.local_port);
    t->dst_port = htons(tcp.remote_port);
    t->seq = htonl(tcp.snd_nxt);
    t->ack = htonl(tcp.rcv_nxt);
    t->data_off = (sizeof(tcp_hdr_t) / 4) << 4;
    t->flags = flags;
    // Advertise what we can actually take. A fixed 8KB window made the peer
    // stop and wait for an acknowledgement every 8KB -- on a 100KB image that
    // is a dozen wasted round trips. 64KB is the most that fits in the field
    // without window scaling, and our buffer is four times that.
    {
        int space = tcp_rx_space() + tcp.rx_head;   // after a compaction
        if (space > 65535) space = 65535;
        if (space < 0) space = 0;
        t->window = htons((uint16_t)space);
    }
    t->checksum = 0;
    t->urgent = 0;
    if (len) nmemcpy(seg + sizeof(tcp_hdr_t), data, len);

    uint8_t pseudo[12];
    uint32_t s = htonl(our_ip), d = htonl(tcp.remote_ip);
    nmemcpy(pseudo, &s, 4); nmemcpy(pseudo + 4, &d, 4);
    pseudo[8] = 0; pseudo[9] = IPPROTO_TCP;
    uint16_t l = htons((uint16_t)(sizeof(tcp_hdr_t) + len));
    nmemcpy(pseudo + 10, &l, 2);
    uint32_t sum = csum_partial(pseudo, 12, 0);
    sum = csum_partial(seg, sizeof(tcp_hdr_t) + len, sum);
    t->checksum = htons(csum_fold(sum));

    ip_send(tcp.remote_ip, IPPROTO_TCP, seg, sizeof(tcp_hdr_t) + len);
}

static void tcp_input(uint32_t srcip, const uint8_t *seg, int len) {
    if (tcp.state == TCP_CLOSED) return;
    if (len < (int)sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *t = (const tcp_hdr_t *)seg;
    if (srcip != tcp.remote_ip) return;
    if (ntohs(t->dst_port) != tcp.local_port) return;
    if (ntohs(t->src_port) != tcp.remote_port) return;

    uint8_t flags = t->flags;
    uint32_t seq = ntohl(t->seq);
    uint32_t ack = ntohl(t->ack);
    int hlen = (t->data_off >> 4) * 4;
    int dlen = len - hlen;
    const uint8_t *data = seg + hlen;

    if (flags & TCP_RST) { tcp.state = TCP_CLOSED; tcp.remote_closed = 1; return; }

    if (tcp.state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            tcp.rcv_nxt = seq + 1;
            tcp.snd_nxt = ack;            // our SYN acknowledged
            tcp.state = TCP_ESTABLISHED;
            tcp_out(TCP_ACK, NULL, 0);
        }
        return;
    }

    if (tcp.state == TCP_ESTABLISHED || tcp.state == TCP_CLOSING) {
        // In-order data only (a client fetch over a LAN needs no reassembly).
        if (dlen > 0 && seq == tcp.rcv_nxt) {
            // Slide the unread bytes back to the front only when the room
            // behind them has run out -- not on every read.
            if (tcp_rx_space() < dlen && tcp.rx_head > 0) {
                nmemmove_down(tcp.rx, tcp.rx + tcp.rx_head, tcp.rx_len);
                tcp.rx_head = 0;
            }
            int space = tcp_rx_space();
            int take = dlen < space ? dlen : space;
            nmemcpy(tcp.rx + tcp.rx_head + tcp.rx_len, data, take);
            tcp.rx_len += take;
            tcp.rcv_nxt += dlen;
            tcp_out(TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            tcp.rcv_nxt += 1;
            tcp.remote_closed = 1;
            tcp_out(TCP_ACK, NULL, 0);
        }
    }
}

static int tcp_connect(uint32_t ip, uint16_t port) {
    nmemset(&tcp, 0, sizeof(tcp));
    tcp.remote_ip = ip;
    tcp.remote_port = port;
    tcp.local_port = 40000 + (uint16_t)(now_ms() & 0x1FFF);
    tcp.snd_nxt = 0x1000 + (uint32_t)(now_ms() & 0xFFFF);
    tcp.state = TCP_SYN_SENT;

    for (int tries = 0; tries < 5; tries++) {
        uint32_t isn = tcp.snd_nxt;
        tcp_out(TCP_SYN, NULL, 0);
        tcp.snd_nxt = isn + 1;          // SYN consumes one sequence number
        uint64_t deadline = now_ms() + 600;
        while (now_ms() < deadline) {
            net_poll();
            if (tcp.state == TCP_ESTABLISHED) return 1;
            if (tcp.state == TCP_CLOSED) return 0;
        }
        tcp.snd_nxt = isn;              // retransmit with the same ISN
    }
    return 0;
}

static void tcp_send_data(const void *data, int len) {
    const uint8_t *p = data;
    while (len > 0) {
        int chunk = len > 1460 ? 1460 : len;
        tcp_out(TCP_ACK | TCP_PSH, p, chunk);
        tcp.snd_nxt += chunk;
        p += chunk; len -= chunk;
        // Pump once so the ACK for this segment is absorbed promptly.
        net_poll();
    }
}

static void tcp_close(void);

// --- the stream interface used by tls.c ----------------------------------
//
// TLS needs to read a length-prefixed record, then the next one, which is not
// something net_http_get's "drain everything then look at it" shape can do.
// These expose the same single connection as a byte stream: fill waits for
// bytes to arrive, peek looks at them, consume drops the ones that have been
// used. Everything stays inside net.c because the connection state does.

int net_tcp_open(uint32_t ip, uint16_t port) { return tcp_connect(ip, port); }

int net_tcp_write(const void *data, int len) {
    if (tcp.state != TCP_ESTABLISHED) return -1;
    tcp_send_data(data, len);
    return len;
}

int net_tcp_avail(void) { return tcp.rx_len; }
const uint8_t *net_tcp_peek(void) { return tcp.rx + tcp.rx_head; }
int net_tcp_closed(void) { return tcp.remote_closed || tcp.state == TCP_CLOSED; }

void net_tcp_consume(int n) {
    if (n <= 0) return;
    if (n > tcp.rx_len) n = tcp.rx_len;
    tcp.rx_head += n;
    tcp.rx_len  -= n;
    if (tcp.rx_len == 0) tcp.rx_head = 0;   // empty: start from the front again
}

int net_tcp_fill(int want, uint32_t timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;
    while (tcp.rx_len < want) {
        if (net_tcp_closed()) break;
        if (now_ms() >= deadline) break;
        net_poll();
    }
    return tcp.rx_len;
}

void net_tcp_shutdown(void) { tcp_close(); }

static void tcp_close(void) {
    if (tcp.state == TCP_ESTABLISHED || tcp.state == TCP_CLOSING) {
        tcp_out(TCP_FIN | TCP_ACK, NULL, 0);
        tcp.snd_nxt += 1;
    }
    tcp.state = TCP_CLOSED;
}

// ==========================================================================
//  DNS
// ==========================================================================

// Answers we already have. Small, because a page draws on a handful of names;
// timed, because an answer is only true for a while.
#define DNS_SLOTS 16
#define DNS_TTL_MS 120000
static struct {
    char     name[96];
    uint32_t ip;
    uint64_t until;
} dns_cache[DNS_SLOTS];
static int dns_next;

static int dns_name_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static int dns_cached(const char *name, uint32_t *ip) {
    for (int i = 0; i < DNS_SLOTS; i++)
        if (dns_cache[i].ip && dns_cache[i].until > now_ms() &&
            dns_name_eq(dns_cache[i].name, name)) {
            *ip = dns_cache[i].ip;
            return 1;
        }
    return 0;
}

static void dns_remember(const char *name, uint32_t ip) {
    int n = 0;
    while (name[n]) n++;
    if (n >= (int)sizeof dns_cache[0].name) return;   // too long to hold
    int slot = dns_next;
    for (int i = 0; i < DNS_SLOTS; i++)               // prefer a free one
        if (!dns_cache[i].ip || dns_cache[i].until <= now_ms()) { slot = i; break; }
    for (int i = 0; i <= n; i++) dns_cache[slot].name[i] = name[i];
    dns_cache[slot].ip = ip;
    dns_cache[slot].until = now_ms() + DNS_TTL_MS;
    dns_next = (dns_next + 1) % DNS_SLOTS;
}

static int dns_query(const char *name, uint32_t *ip);

int net_resolve(const char *name, uint32_t *ip) {
    if (!up) return 0;
    // Accept a dotted-quad without a lookup.
    if (net_parse_ip(name, ip)) return 1;
    if (dns_cached(name, ip)) return 1;
    if (!dns_query(name, ip)) return 0;
    dns_remember(name, *ip);
    return 1;
}

static int dns_query(const char *name, uint32_t *ip) {

    uint8_t q[512];
    uint16_t txid = (uint16_t)(now_ms() & 0xFFFF) ^ 0xA5A5;
    int n = 0;
    uint16_t v;
    v = htons(txid);      nmemcpy(q + n, &v, 2); n += 2;
    v = htons(0x0100);    nmemcpy(q + n, &v, 2); n += 2;   // recursion desired
    v = htons(1);         nmemcpy(q + n, &v, 2); n += 2;   // QDCOUNT
    v = 0;                nmemcpy(q + n, &v, 2); n += 2;   // ANCOUNT
    v = 0;                nmemcpy(q + n, &v, 2); n += 2;   // NSCOUNT
    v = 0;                nmemcpy(q + n, &v, 2); n += 2;   // ARCOUNT
    // QNAME: length-prefixed labels
    const char *p = name;
    while (*p) {
        const char *dot = p;
        int l = 0;
        while (dot[l] && dot[l] != '.') l++;
        q[n++] = (uint8_t)l;
        for (int i = 0; i < l; i++) q[n++] = (uint8_t)p[i];
        p += l;
        if (*p == '.') p++;
    }
    q[n++] = 0;                                            // root label
    v = htons(1); nmemcpy(q + n, &v, 2); n += 2;           // QTYPE A
    v = htons(1); nmemcpy(q + n, &v, 2); n += 2;           // QCLASS IN

    uint16_t sport = 50000 + (uint16_t)(now_ms() & 0x1FFF);
    for (int tries = 0; tries < 3; tries++) {
        udp_have = 0;
        if (udp_send(dns_ip, sport, 53, q, n) != 0) return 0;
        uint64_t deadline = now_ms() + 1200;
        while (now_ms() < deadline) {
            net_poll();
            if (udp_have && udp_dport == sport && udp_sip == dns_ip) {
                // Parse the answer section for the first A record.
                uint8_t *r = udp_data;
                int rl = udp_dlen;
                if (rl < 12) break;
                uint16_t ancount = ntohs(*(uint16_t *)(r + 6));
                int off = 12;
                // skip QNAME
                while (off < rl && r[off]) off += r[off] + 1;
                off += 1 + 4;               // zero byte + QTYPE + QCLASS
                for (int a = 0; a < ancount && off + 12 <= rl; a++) {
                    // NAME (pointer 0xC0.. or labels)
                    if ((r[off] & 0xC0) == 0xC0) off += 2;
                    else { while (off < rl && r[off]) off += r[off] + 1; off += 1; }
                    uint16_t atype = ntohs(*(uint16_t *)(r + off));
                    uint16_t rdlen = ntohs(*(uint16_t *)(r + off + 8));
                    off += 10;
                    if (atype == 1 && rdlen == 4) {
                        uint32_t a4;
                        nmemcpy(&a4, r + off, 4);
                        *ip = ntohl(a4);
                        return 1;
                    }
                    off += rdlen;
                }
                break;
            }
        }
    }
    return 0;
}

// ==========================================================================
//  fetching on its own stack
// ==========================================================================

enum { AF_IDLE, AF_RUN, AF_DONE };

// How long the fetch task may hold the processor before handing it back. Short
// enough that a window redrawing at 60Hz does not visibly stutter.
#define AF_SLICE_US 4000
#define AF_STACK    (64 * 1024)

// Set while the fetch task is the one holding the processor, and the moment
// it has to hand it back.
static int      af_running;
static uint64_t af_slice_end;

static struct {
    int      state;
    char     host[160];
    char     path[1024];
    uint16_t port;
    int      tls, raw;
    int      result;          // bytes, or -1
    char     body[4096];      // a form being posted; empty for a GET
    int      blen;
    char     xhdr[2048];      // headers the browser wrote, Cookie among them
    task_t  *task;
} af;

static uint8_t af_buf[1024 * 1024];

// How much of the answer is in hand. The receive buffer is the honest measure
// while it is still arriving; the final count replaces it at the end.
static int af_progress;

static void af_entry(void) {
    for (;;) {
        uint32_t ip;
        int r = -1;
        if (net_resolve(af.host, &ip)) {
            r = af.tls
              ? tls_https_get(af.host, ip, af.port, af.path,
                              (char *)af_buf, (int)sizeof af_buf, af.raw,
                              af.blen ? af.body : 0, af.blen,
                              af.xhdr[0] ? af.xhdr : 0)
              : net_http_get(af.host, ip, af.port, af.path,
                             (char *)af_buf, (int)sizeof af_buf, af.raw,
                             af.blen ? af.body : 0, af.blen,
                             af.xhdr[0] ? af.xhdr : 0);
        }
        af.result = r;
        af.state = AF_DONE;
        // Nothing more to do until somebody sets up the next one.
        task_yield_back();
    }
}

int net_fetch_busy(void) { return af.state == AF_RUN; }

int net_fetch_start(const char *host, const char *path, uint16_t port,
                    int tls, int keep_headers, const char *body, int blen,
                    const char *xhdr, int xhdrlen) {
    if (af.state == AF_RUN) return 0;
    if (!af.task) {
        af.task = task_create_sized("fetch", af_entry, AF_STACK);
        if (!af.task) return 0;
    }
    int i = 0;
    for (; host[i] && i < (int)sizeof af.host - 1; i++) af.host[i] = host[i];
    af.host[i] = 0;
    for (i = 0; path[i] && i < (int)sizeof af.path - 1; i++) af.path[i] = path[i];
    af.path[i] = 0;
    af.port = port;
    af.tls = tls;
    af.raw = keep_headers;
    af.xhdr[0] = 0;
    if (xhdr && xhdrlen > 0) {
        if (xhdrlen > (int)sizeof af.xhdr - 1) xhdrlen = (int)sizeof af.xhdr - 1;
        for (int k = 0; k < xhdrlen; k++) af.xhdr[k] = xhdr[k];
        af.xhdr[xhdrlen] = 0;
    }
    af.blen = 0;
    if (body && blen > 0) {
        if (blen > (int)sizeof af.body) blen = (int)sizeof af.body;
        for (int k = 0; k < blen; k++) af.body[k] = body[k];
        af.blen = blen;
    }
    af.result = -1;
    af_progress = 0;
    af.state = AF_RUN;
    return 1;
}

int net_fetch_poll(int *progress) {
    if (af.state == AF_IDLE) return -1;
    if (af.state == AF_RUN) {
        af_slice_end = now_us() + AF_SLICE_US;
        af_running = 1;
        task_resume(af.task);
        af_running = 0;
        if (af.state == AF_RUN) {
            if (progress) *progress = tcp.rx_len > af_progress
                                    ? tcp.rx_len : af_progress;
            return NET_FETCH_PENDING;
        }
    }
    af_progress = af.result > 0 ? af.result : 0;
    if (progress) *progress = af_progress;
    return af.result;
}

int net_fetch_take(char *dst, int max) {
    int n = af.result;
    if (n > max) n = max;
    if (n > 0) nmemcpy(dst, af_buf, n);
    af.state = AF_IDLE;
    return af.result;
}

// ==========================================================================
//  the request log
// ==========================================================================

static struct net_log_ent log_ring[NET_LOG_SLOTS];
static int log_next = 0;        // where the next entry goes
static int log_count = 0;       // how many slots have ever been filled

uint32_t net_ms(void) { return (uint32_t)now_ms(); }

static void log_copy(char *dst, int cap, const char *src) {
    int i = 0;
    if (src) while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void net_log_add(const char *host, const char *path, int bytes, int status,
                 uint32_t start_ms, unsigned flags) {
    struct net_log_ent *e = &log_ring[log_next];
    e->start_ms = start_ms;
    e->dur_ms   = (uint32_t)(now_ms() - start_ms);
    e->bytes    = bytes;
    e->status   = (int16_t)status;
    e->flags    = (uint8_t)flags;
    e->pad      = 0;
    log_copy(e->host, sizeof e->host, host);
    log_copy(e->path, sizeof e->path, path);
    log_next = (log_next + 1) % NET_LOG_SLOTS;
    if (log_count < NET_LOG_SLOTS) log_count++;
}

int net_log_read(struct net_log_ent *out, int max) {
    int n = log_count < max ? log_count : max;
    // Oldest first: the ring's oldest is the slot just after the newest once
    // it has wrapped, and slot zero before that.
    int first = (log_count == NET_LOG_SLOTS) ? log_next : 0;
    first = (first + (log_count - n)) % NET_LOG_SLOTS;
    for (int i = 0; i < n; i++)
        out[i] = log_ring[(first + i) % NET_LOG_SLOTS];
    return n;
}

void net_log_clear(void) { log_next = log_count = 0; }

// ==========================================================================
//  HTTP
// ==========================================================================

int net_http_get(const char *host, uint32_t ip, uint16_t port,
                 const char *path, char *buf, int max, int keep_headers,
                 const char *post, int postlen, const char *xhdr) {
    if (!up) return -1;
    uint64_t started = now_ms();
    if (!tcp_connect(ip, port)) {
        net_log_add(host, path, -1, 0, (uint32_t)started, NET_LOG_FAIL);
        return -1;
    }

    char req[2816];      // the cookies a site sets can be long
    int n = 0;
    /* The two header names alone are sixty-five characters, so a
     * sixty-four byte buffer cut this in half and the length never
     * reached the server -- which read the body as empty. */
    char clen[128];
    clen[0] = 0;
    if (postlen > 0) {
        // A form is what a body is, here and in the TLS path both.
        const char *pre = "Content-Type: application/x-www-form-urlencoded\r\n"
                          "Content-Length: ";
        int c = 0;
        while (*pre && c < (int)sizeof clen - 16) clen[c++] = *pre++;
        char d[12];
        int dn = 0, v = postlen;
        do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (dn) clen[c++] = d[--dn];
        clen[c++] = '\r'; clen[c++] = '\n';
        clen[c] = 0;
    }
    const char *parts[] = { postlen > 0 ? "POST " : "GET ", path,
                            " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\n"
                            "Accept-Encoding: gzip, deflate, identity\r\n"
                            "Accept: text/html,application/xhtml+xml,text/css,image/png,image/jpeg,image/gif,image/bmp,*/*;q=0.5\r\n"
                            "User-Agent: xyuos\r\n", clen,
                            xhdr ? xhdr : "", "\r\n" };
    for (int i = 0; i < 8; i++) {
        const char *s = parts[i];
        while (*s && n < (int)sizeof(req) - 1) req[n++] = *s++;
    }
    tcp_send_data(req, n);
    if (postlen > 0) tcp_send_data(post, postlen);

    // Drain until the peer closes or we time out with no progress.
    uint64_t last = now_ms();
    int seen = tcp.rx_len;
    while (!tcp.remote_closed) {
        net_poll();
        if (tcp.rx_len != seen) { seen = tcp.rx_len; last = now_ms(); }
        if (now_ms() - last > 4000) break;         // stall timeout
        if (tcp.rx_len >= (int)sizeof(tcp.rx)) break;
    }
    tcp_close();

    // Strip headers: find the blank line. A caller that asked for them keeps
    // everything from the status line on.
    uint8_t *rx = tcp.rx + tcp.rx_head;   // written into when the body is rejoined
    int status = 0;
    if (tcp.rx_len > 12 && rx[0] == 'H')
        status = (rx[9] - '0') * 100 + (rx[10] - '0') * 10 + (rx[11] - '0');

    int body = 0;
    for (int i = 0; i + 3 < tcp.rx_len; i++) {
        if (rx[i] == '\r' && rx[i+1] == '\n' &&
            rx[i+2] == '\r' && rx[i+3] == '\n') { body = i + 4; break; }
    }
    int hdr = body;

    // The body may have arrived in pieces, each behind its own length in
    // hexadecimal. Undone in place -- the headers sit in front of it and are
    // not disturbed -- and then the header that said so is renamed, because a
    // caller reading it would otherwise be told to undo it a second time.
    // This is the same treatment the TLS path gives it, from the same code.
    int blen = tcp.rx_len - hdr;
    if (blen < 0) blen = 0;
    if (http_is_chunked(rx, hdr)) {
        blen = http_dechunk(rx + hdr, blen);
        http_mark_dechunked(rx, hdr);
    }

    int n2 = keep_headers ? hdr + blen : blen;
    const uint8_t *src = keep_headers ? rx : rx + hdr;
    if (n2 > max) n2 = max;
    nmemcpy(buf, src, n2);
    net_log_add(host, path, blen, status, (uint32_t)started, 0);
    return n2;
}

// ==========================================================================
//  DHCP
// ==========================================================================

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} dhcp_msg_t;

#define DHCP_MAGIC 0x63825363

static uint32_t dhcp_xid;

static int dhcp_build(dhcp_msg_t *m, uint8_t msgtype, uint32_t req_ip, uint32_t server_ip) {
    nmemset(m, 0, sizeof(*m));
    m->op = 1; m->htype = 1; m->hlen = 6;
    m->xid = dhcp_xid;               // already network order (opaque)
    m->flags = htons(0x8000);        // ask for broadcast replies
    nmemcpy(m->chaddr, our_mac, 6);
    m->magic = htonl(DHCP_MAGIC);
    int o = 0;
    m->options[o++] = 53; m->options[o++] = 1; m->options[o++] = msgtype;
    if (req_ip) {
        m->options[o++] = 50; m->options[o++] = 4;
        uint32_t v = htonl(req_ip); nmemcpy(m->options + o, &v, 4); o += 4;
    }
    if (server_ip) {
        m->options[o++] = 54; m->options[o++] = 4;
        uint32_t v = htonl(server_ip); nmemcpy(m->options + o, &v, 4); o += 4;
    }
    // Parameter request list: mask, router, DNS.
    m->options[o++] = 55; m->options[o++] = 3;
    m->options[o++] = 1; m->options[o++] = 3; m->options[o++] = 6;
    m->options[o++] = 255;           // end
    // Send the fixed part + magic + options used.
    return (int)(sizeof(dhcp_msg_t) - sizeof(m->options) + o);
}

static uint8_t dhcp_opt(const dhcp_msg_t *m, int mlen, uint8_t code, uint8_t *val, int maxval) {
    int optlen = mlen - (int)(sizeof(dhcp_msg_t) - sizeof(m->options));
    const uint8_t *o = m->options;
    int i = 0;
    while (i < optlen) {
        uint8_t c = o[i++];
        if (c == 0) continue;
        if (c == 255) break;
        uint8_t l = o[i++];
        if (c == code) {
            int cp = l < maxval ? l : maxval;
            nmemcpy(val, o + i, cp);
            return l;
        }
        i += l;
    }
    return 0;
}

static volatile int dhcp_have = 0;
static dhcp_msg_t   dhcp_reply;
static int          dhcp_reply_len;

static int dhcp_run(void) {
    dhcp_xid = htonl(0xDEADBEEF ^ (uint32_t)now_ms());

    // --- DISCOVER ---
    dhcp_msg_t msg;
    int mlen = dhcp_build(&msg, 1 /*DISCOVER*/, 0, 0);
    uint32_t offered = 0, server = 0;

    for (int tries = 0; tries < 6 && !offered; tries++) {
        dhcp_have = 0;
        udp_send_raw(BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, &msg, mlen);
        uint64_t deadline = now_ms() + 1000;
        while (now_ms() < deadline) {
            net_poll();
            if (dhcp_have) {
                uint8_t t = 0;
                dhcp_opt(&dhcp_reply, dhcp_reply_len, 53, &t, 1);
                if (t == 2 /*OFFER*/) {
                    offered = ntohl(dhcp_reply.yiaddr);
                    uint8_t sv[4] = {0,0,0,0};
                    dhcp_opt(&dhcp_reply, dhcp_reply_len, 54, sv, 4);
                    server = (sv[0]<<24)|(sv[1]<<16)|(sv[2]<<8)|sv[3];
                    break;
                }
                dhcp_have = 0;
            }
        }
    }
    if (!offered) { kprintf("dhcp: no offer\n"); return 0; }

    // --- REQUEST ---
    mlen = dhcp_build(&msg, 3 /*REQUEST*/, offered, server);
    for (int tries = 0; tries < 6; tries++) {
        dhcp_have = 0;
        udp_send_raw(BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, &msg, mlen);
        uint64_t deadline = now_ms() + 1000;
        while (now_ms() < deadline) {
            net_poll();
            if (dhcp_have) {
                uint8_t t = 0;
                dhcp_opt(&dhcp_reply, dhcp_reply_len, 53, &t, 1);
                if (t == 5 /*ACK*/) {
                    our_ip = ntohl(dhcp_reply.yiaddr);
                    uint8_t v[4];
                    if (dhcp_opt(&dhcp_reply, dhcp_reply_len, 1, v, 4))
                        net_mask = (v[0]<<24)|(v[1]<<16)|(v[2]<<8)|v[3];
                    if (dhcp_opt(&dhcp_reply, dhcp_reply_len, 3, v, 4))
                        gw_ip = (v[0]<<24)|(v[1]<<16)|(v[2]<<8)|v[3];
                    if (dhcp_opt(&dhcp_reply, dhcp_reply_len, 6, v, 4))
                        dns_ip = (v[0]<<24)|(v[1]<<16)|(v[2]<<8)|v[3];
                    return 1;
                }
                dhcp_have = 0;
            }
        }
    }
    kprintf("dhcp: no ack\n");
    return 0;
}

// ==========================================================================
//  receive path
// ==========================================================================

static void handle_arp(const uint8_t *p, int len) {
    if (len < (int)sizeof(arp_pkt_t)) return;
    const arp_pkt_t *a = (const arp_pkt_t *)p;
    uint32_t spa, tpa;
    nmemcpy(&spa, a->spa, 4); spa = ntohl(spa);
    nmemcpy(&tpa, a->tpa, 4); tpa = ntohl(tpa);
    arp_store(spa, a->sha);
    if (ntohs(a->oper) == 1 && our_ip && tpa == our_ip)
        arp_send(2, spa, a->sha);       // reply to a request for us
}

static void handle_udp(uint32_t srcip, const uint8_t *p, int len) {
    if (len < (int)sizeof(udp_hdr_t)) return;
    const udp_hdr_t *u = (const udp_hdr_t *)p;
    uint16_t sport = ntohs(u->src_port);
    uint16_t dport = ntohs(u->dst_port);
    int dlen = (int)ntohs(u->length) - sizeof(udp_hdr_t);
    if (dlen < 0 || dlen > len - (int)sizeof(udp_hdr_t)) dlen = len - sizeof(udp_hdr_t);
    const uint8_t *data = p + sizeof(udp_hdr_t);

    if (dport == 68) {                  // DHCP client
        if (dlen > (int)sizeof(dhcp_reply)) dlen = sizeof(dhcp_reply);
        nmemcpy(&dhcp_reply, data, dlen);
        dhcp_reply_len = dlen;
        dhcp_have = 1;
        return;
    }
    // generic capture (DNS)
    if (dlen > (int)sizeof(udp_data)) dlen = sizeof(udp_data);
    nmemcpy(udp_data, data, dlen);
    udp_dlen = dlen;
    udp_sport = sport;
    udp_dport = dport;
    udp_sip = srcip;
    udp_have = 1;
}

static void handle_icmp(uint32_t srcip, const uint8_t *p, int len) {
    if (len < (int)sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *ic = (const icmp_hdr_t *)p;
    if (ic->type == 8) {                // echo request -> reply
        uint8_t rep[1500];
        if (len > (int)sizeof(rep)) return;
        nmemcpy(rep, p, len);
        icmp_hdr_t *r = (icmp_hdr_t *)rep;
        r->type = 0; r->checksum = 0;
        r->checksum = htons(ip_checksum(rep, len));
        ip_send(srcip, IPPROTO_ICMP, rep, len);
    } else if (ic->type == 0) {         // echo reply
        icmp_r_from = srcip;
        icmp_r_id = ntohs(ic->id);
        icmp_r_seq = ntohs(ic->seq);
        icmp_have = 1;
    }
}

static void handle_ip(const uint8_t *p, int len) {
    if (len < (int)sizeof(ip_hdr_t)) return;
    const ip_hdr_t *ih = (const ip_hdr_t *)p;
    if ((ih->ver_ihl >> 4) != 4) return;
    int ihl = (ih->ver_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;
    uint32_t src = ntohl(ih->src);
    uint32_t dst = ntohl(ih->dst);
    // Accept traffic to us or to broadcast (DHCP).
    if (our_ip && dst != our_ip && dst != 0xFFFFFFFF) return;
    int plen = (int)ntohs(ih->total_len) - ihl;
    if (plen < 0 || plen > len - ihl) plen = len - ihl;
    const uint8_t *payload = p + ihl;
    switch (ih->proto) {
        case IPPROTO_ICMP: handle_icmp(src, payload, plen); break;
        case IPPROTO_UDP:  handle_udp(src, payload, plen); break;
        case IPPROTO_TCP:  tcp_input(src, payload, plen); break;
        default: break;
    }
}

void net_rx(const uint8_t *frame, uint16_t len) {
    if (len < (int)sizeof(eth_hdr_t)) return;
    rx_frames++;
    const eth_hdr_t *e = (const eth_hdr_t *)frame;
    uint16_t type = ntohs(e->type);
    const uint8_t *payload = frame + sizeof(eth_hdr_t);
    int plen = len - sizeof(eth_hdr_t);
    if (type == ETH_ARP) handle_arp(payload, plen);
    else if (type == ETH_IP) handle_ip(payload, plen);
}

/* net_poll is the single place every wait in this file and in tls.c passes
 * through, which makes it the one place that needs to know about slices --
 * af_running and af_slice_end are set by the async section above. */
void net_poll(void) {
    nic_poll();
    if (af_running && now_us() >= af_slice_end) task_yield_back();
}

// ==========================================================================
//  public entry points
// ==========================================================================

int net_parse_ip(const char *s, uint32_t *out) {
    uint32_t parts[4]; int pi = 0, val = 0, digits = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0'); digits++;
            if (val > 255) return 0;
        } else if (*p == '.' || *p == 0) {
            if (!digits || pi > 3) return 0;
            parts[pi++] = val; val = 0; digits = 0;
            if (*p == 0) break;
        } else return 0;
    }
    if (pi != 4) return 0;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

int net_up(void) {
    if (up) return 1;
    if (!have_nic) {
        if (!nic_init()) { kprintf("net: no NIC\n"); return 0; }
        nmemcpy(our_mac, nic_mac(), 6);
        have_nic = 1;
    }

    // Wait for the physical link (auto-negotiation can take a few seconds).
    if (!nic_link()) {
        kprintf("net: waiting for link");
        uint64_t deadline = now_ms() + 9000;
        while (now_ms() < deadline && !nic_link()) {
            uint64_t next = now_ms() + 1000;
            while (now_ms() < next) net_poll();
            kprintf(".");
        }
        kprintf(nic_link() ? " up\n" : " no link\n");
    }
    if (!nic_link()) {
        kprintf("net: link down -- check the Ethernet cable\n");
        return 0;
    }

    if (dhcp_run()) {
        up = 1;
        kprintf("net: %d.%d.%d.%d/%d gw %d.%d.%d.%d dns %d.%d.%d.%d\n",
                (our_ip>>24)&0xFF,(our_ip>>16)&0xFF,(our_ip>>8)&0xFF,our_ip&0xFF,
                __builtin_popcount(net_mask),
                (gw_ip>>24)&0xFF,(gw_ip>>16)&0xFF,(gw_ip>>8)&0xFF,gw_ip&0xFF,
                (dns_ip>>24)&0xFF,(dns_ip>>16)&0xFF,(dns_ip>>8)&0xFF,dns_ip&0xFF);
        return 1;
    }
    kprintf("net: DHCP failed (tx=%d rx=%d) -- no reply from a DHCP server\n",
            tx_frames, rx_frames);
    return 0;
}

int net_is_up(void) { return up; }

void net_config(uint32_t *ip, uint32_t *gw, uint32_t *mask, uint32_t *dns) {
    if (ip) *ip = our_ip;
    if (gw) *gw = gw_ip;
    if (mask) *mask = net_mask;
    if (dns) *dns = dns_ip;
}

const uint8_t *net_mac(void) { return have_nic ? our_mac : NULL; }
