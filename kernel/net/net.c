#include "net.h"
#include "../kernel/clock.h"
#include "http.h"
#include "nic.h"
#include "tls.h"
#include "../kernel/kio.h"
#include "../kernel/task.h"
#include "../kernel/process.h"
#include "../mm/heap.h"
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

// Set while a received packet is being handled. An answer sent from in
// there must not wait for anything: waiting means polling the card, and the
// card is in the middle of handing us this very packet. So an address not yet
// known is asked for and the answer is not waited for -- the packet is lost,
// and TCP sends it again once the address is in the cache.
static int rx_depth;

static int arp_resolve(uint32_t ip, uint8_t *mac) {
    if (arp_lookup(ip, mac)) return 1;
    if (rx_depth) { arp_send(1, ip, BCAST_MAC); return 0; }
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

static int is_loop(uint32_t ip) { return (ip >> 24) == 127; }

// --- loopback -----------------------------------------------------------------
//
// A packet to 127.x.x.x, or to our own address, never reaches the card: it
// waits here as a whole IP packet and net_poll() hands it to the input side,
// exactly as if it had come off the wire. That is what lets a program talk to
// a server on the same machine -- and test one without a second computer.

#define LOOP_Q 64
typedef struct { uint16_t len; uint8_t pkt[1500]; } loop_pkt_t;
static loop_pkt_t *loopq;
static int loop_head, loop_n;

static int loop_send(uint32_t dstip, uint8_t proto, const void *payload, int len) {
    if (len > 1480) return -1;
    if (!loopq) {
        loopq = kmalloc(sizeof(loop_pkt_t) * LOOP_Q);
        if (!loopq) return -1;
    }
    if (loop_n == LOOP_Q) return -1;               // full: lost, as on a wire
    loop_pkt_t *q = &loopq[(loop_head + loop_n) % LOOP_Q];
    ip_hdr_t *ih = (ip_hdr_t *)q->pkt;
    ih->ver_ihl = 0x45;
    ih->tos = 0;
    ih->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + len));
    ih->id = htons(ip_id++);
    ih->flags_frag = 0;
    ih->ttl = 64;
    ih->proto = proto;
    ih->checksum = 0;
    ih->src = htonl(is_loop(dstip) ? dstip : our_ip);
    ih->dst = htonl(dstip);
    ih->checksum = htons(ip_checksum(ih, sizeof(ip_hdr_t)));
    nmemcpy(q->pkt + sizeof(ip_hdr_t), payload, len);
    q->len = (uint16_t)(sizeof(ip_hdr_t) + len);
    loop_n++;
    return 0;
}

static int ip_send(uint32_t dstip, uint8_t proto, const void *payload, int len) {
    if (is_loop(dstip) || (our_ip && dstip == our_ip)) return loop_send(dstip, proto, payload, len);
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

// Where a reply lands: a box for each port somebody is waiting on.
//
// There used to be one place, shared. Lookups do overlap -- a page names a
// new host and all four fetch slots ask for it together -- and each answer
// then overwrote the one before it. The slots whose answers were lost sat out
// a 1.2-second timeout and asked again: measured on Wikipedia, one to five
// and a half seconds for every new host.
//
// A box a lookup never gave back -- its program gone mid-question -- is taken
// over once it is older than any lookup can last, three tries at 2.4 seconds
// each, so a lost one costs nothing for good.
#define UDP_BOXES     8
#define UDP_BOX_STALE 10000     // ms
typedef struct {
    uint16_t port;              // 0 when free
    int      have;              // a datagram is waiting in data[]
    uint32_t sip;
    int      len;
    uint64_t since;             // when it was claimed
    uint8_t  data[1500];
} udp_box_t;
static udp_box_t udp_boxes[UDP_BOXES];

static udp_box_t *udp_box_open(uint16_t port) {
    for (int i = 0; i < UDP_BOXES; i++) {
        udp_box_t *b = &udp_boxes[i];
        if (b->port && now_ms() - b->since < UDP_BOX_STALE) continue;
        b->port = port;
        b->have = 0;
        b->since = now_ms();
        return b;
    }
    return NULL;
}

static void udp_box_close(udp_box_t *b) { b->port = 0; b->have = 0; }

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
//  TCP
// ==========================================================================
//
// Several connections at once. It used to be exactly one -- a single global
// struct -- which was enough to fetch a page and hopeless for the twenty
// files that follow it: they went one at a time, each waiting for the last.
//
// Both ends now: connections we open, and ones that come in to a port a
// program listens on (see "listening" below).
//
// A segment lost on the way used to be expensive out of all proportion. The
// ones behind it were thrown away without a word, so the sender learned of
// the loss only from its own retransmission timer -- a second or more, and
// doubling each time it happened again -- and then had to send everything
// after the hole a second time. In QEMU nothing is ever lost and none of it
// showed. Over a phone it made a page take minutes.
//
// Now every data segment is answered. One past a gap repeats the last
// acknowledgement, and three repeats are the standard signal for the sender
// to resend the missing segment at once (fast retransmit). And what arrived
// past the gap is kept: written into the receive buffer at its place, its
// range noted, so that when the hole is filled the acknowledgement jumps over
// all of it and nothing is sent twice.

#define TCP_CONNS     32        // the browser's, and room for programs' sockets
#define TCP_HELD      8         // ranges kept past a gap, per connection
#define TCP_TX_CAP    (64 * 1024)
#define TCP_MSS       1460
#define TCP_RTO_SYN   600       // ms; a SYN is resent sooner than data
#define TCP_RTO_INIT  1000
#define TCP_RTO_MAX   8000
#define TCP_RETRIES   8
#define TCP_LINGER_MS 5000      // how long a closed handle's FIN may take to land
#define TCP_LISTENERS 8
#define TCP_BACKLOG   16

// TCP_CLOSING: our FIN is out (or about to be); the peer may still send.
enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_CLOSING, TCP_SYN_RCVD };

typedef struct {
    int      state;
    uint32_t remote_ip, local_ip;
    uint16_t remote_port, local_port;

    // --- receiving
    uint32_t rcv_nxt;         // next sequence we expect
    uint8_t *rx;              // from the kernel heap, sized by the opener
    int      rx_cap;
    int      rx_head;         // where the unread bytes start
    int      rx_len;          // how many there are
    int      remote_closed;
    // Data that arrived past a gap: [held_lo, held_hi) in sequence numbers,
    // already in rx at rx_head + rx_len + (held_lo - rcv_nxt).
    int      nheld;
    uint32_t held_lo[TCP_HELD], held_hi[TCP_HELD];
    int      adv_win;         // the window the peer was last told about

    // --- sending
    // Everything from snd_una on is kept in tx until it is acknowledged:
    // tx_len bytes from tx_head, a ring. snd_nxt is how far it has been sent
    // (it goes back to snd_una when a timer runs out), snd_max how far it
    // ever went.
    uint32_t iss, snd_una, snd_nxt, snd_max;
    uint8_t *tx;
    int      tx_head, tx_len;
    int      snd_wnd;         // what the peer last said it would take
    int      cwnd;            // what we dare have in flight
    int      mss;             // the peer's largest segment
    int      dupacks;
    int      fin_want;        // FIN goes after the last queued byte
    int      fin_sent;
    uint32_t fin_seq;
    uint64_t rtx_at;          // when to resend; 0 when nothing is out
    int      rto, retries;

    int      err;             // why it ended: 0, or NET_E*
    int      listener;        // the listener it came in on, or -1
    int      orphan;          // nobody holds it: freed once its FIN has gone
    uint64_t orphan_until;
} tcp_conn_t;

static tcp_conn_t conns[TCP_CONNS];

// The ports somebody is listening on, and the connections that came in on
// each and are waiting to be accepted.
static struct {
    int      used;
    uint16_t port;
    uint32_t ip;              // 0 = any address of ours
    int      backlog;
    int      rx_cap;
    int      q[TCP_BACKLOG], qn;
} lsn[TCP_LISTENERS];

// Remember which fetch opened a connection, so an abandoned one can be
// shut without guessing. Defined with the fetch slots.
static void af_note_socket(int h);

// Bytes that have arrived on any connection since boot. Only ever used to
// show that something is happening while a fetch runs -- a progress bar has
// no business knowing which socket the bytes came in on.
static uint32_t tcp_bytes_in;

static tcp_conn_t *conn_of(int h) {
    if (h < 0 || h >= TCP_CONNS) return NULL;
    if (conns[h].state == TCP_CLOSED && conns[h].rx == NULL) return NULL;
    if (conns[h].orphan) return NULL;
    return &conns[h];
}

// Free space behind the unread bytes, which is what the peer is allowed to
// send us next.
static int tcp_rx_space(const tcp_conn_t *c) {
    return c->rx_cap - c->rx_head - c->rx_len;
}

// a - b, for sequence numbers that wrap.
static int32_t seqdiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void tx_copy(const tcp_conn_t *c, int off, uint8_t *dst, int n);

// One segment. `seq` is where its first byte (or its SYN/FIN) sits; a SYN
// carries our largest segment size, so the peer sends full ones. The bytes
// come from `data`, or -- `data` NULL -- from the send ring at `off`, copied
// straight into the segment (a kernel stack is 16 KB, and this can be deep).
static void tcp_seg_from(tcp_conn_t *c, uint8_t flags, uint32_t seq, const void *data, int off, int len) {
    uint8_t seg[sizeof(tcp_hdr_t) + 4 + TCP_MSS];
    if (len > TCP_MSS) len = TCP_MSS;
    int opt = (flags & TCP_SYN) ? 4 : 0;
    tcp_hdr_t *t = (tcp_hdr_t *)seg;
    t->src_port = htons(c->local_port);
    t->dst_port = htons(c->remote_port);
    t->seq = htonl(seq);
    t->ack = htonl(c->rcv_nxt);
    t->data_off = (uint8_t)(((sizeof(tcp_hdr_t) + opt) / 4) << 4);
    t->flags = flags;
    // Advertise what we can actually take. A fixed 8KB window made the peer
    // stop and wait for an acknowledgement every 8KB -- on a 100KB image that
    // is a dozen wasted round trips. 64KB is the most that fits in the field
    // without window scaling.
    {
        int space = c->rx ? tcp_rx_space(c) + c->rx_head : 0;   // after a compaction
        if (space > 65535) space = 65535;
        if (space < 0) space = 0;
        t->window = htons((uint16_t)space);
        c->adv_win = space;
    }
    t->checksum = 0;
    t->urgent = 0;
    if (opt) {
        uint8_t *o = seg + sizeof(tcp_hdr_t);
        o[0] = 2; o[1] = 4; o[2] = (uint8_t)(TCP_MSS >> 8); o[3] = (uint8_t)(TCP_MSS & 0xFF);
    }
    int hl = (int)sizeof(tcp_hdr_t) + opt;
    if (len && data) nmemcpy(seg + hl, data, len);
    else if (len) tx_copy(c, off, seg + hl, len);

    uint8_t pseudo[12];
    uint32_t s = htonl(c->local_ip), d = htonl(c->remote_ip);
    nmemcpy(pseudo, &s, 4); nmemcpy(pseudo + 4, &d, 4);
    pseudo[8] = 0; pseudo[9] = IPPROTO_TCP;
    uint16_t l = htons((uint16_t)(hl + len));
    nmemcpy(pseudo + 10, &l, 2);
    uint32_t sum = csum_partial(pseudo, 12, 0);
    sum = csum_partial(seg, hl + len, sum);
    t->checksum = htons(csum_fold(sum));

    ip_send(c->remote_ip, IPPROTO_TCP, seg, hl + len);
}

static void tcp_seg(tcp_conn_t *c, uint8_t flags, uint32_t seq, const void *data, int len) {
    tcp_seg_from(c, flags, seq, data, 0, len);
}

// An acknowledgement (or a bare flag) at the current position.
static void tcp_out(tcp_conn_t *c, uint8_t flags, const void *data, int len) {
    tcp_seg(c, flags, c->snd_nxt, data, len);
}

// A reset for a segment that belongs to nothing here.
static void tcp_reset_reply(uint32_t to_ip, uint32_t from_ip, uint16_t to_port, uint16_t from_port,
                            uint32_t seq, uint32_t ack, int use_ack) {
    tcp_conn_t tmp;
    nmemset(&tmp, 0, sizeof tmp);
    tmp.remote_ip = to_ip;
    tmp.local_ip = from_ip;
    tmp.remote_port = to_port;
    tmp.local_port = from_port;
    tmp.rcv_nxt = ack;
    tcp_seg(&tmp, (uint8_t)(TCP_RST | (use_ack ? TCP_ACK : 0)), seq, NULL, 0);
}

// Which connection does this segment belong to? The local port alone would
// do for the ones we opened, but a listener's connections all share its
// port, so all four are checked.
static tcp_conn_t *tcp_lookup(uint32_t srcip, uint16_t sport, uint16_t dport) {
    for (int i = 0; i < TCP_CONNS; i++) {
        tcp_conn_t *c = &conns[i];
        if (c->state == TCP_CLOSED) continue;
        if (c->local_port == dport && c->remote_port == sport &&
            c->remote_ip == srcip)
            return c;
    }
    return NULL;
}

// How far past rcv_nxt the held data reaches.
static int tcp_held_reach(const tcp_conn_t *c) {
    int reach = 0;
    for (int i = 0; i < c->nheld; i++) {
        int r = seqdiff(c->held_hi[i], c->rcv_nxt);
        if (r > reach) reach = r;
    }
    return reach;
}

// Slide the unread bytes to the front of the buffer, and the held ones with
// them: they are addressed relative to the unread end.
static void tcp_compact(tcp_conn_t *c) {
    if (c->rx_head == 0) return;
    nmemmove_down(c->rx, c->rx + c->rx_head, c->rx_len + tcp_held_reach(c));
    c->rx_head = 0;
}

static void tcp_held_drop(tcp_conn_t *c, int i) {
    c->nheld--;
    c->held_lo[i] = c->held_lo[c->nheld];
    c->held_hi[i] = c->held_hi[c->nheld];
}

// Note a range that has arrived past the gap, merged with any it touches.
// With the table full it is simply not noted; the sender will send it again.
static void tcp_hold(tcp_conn_t *c, uint32_t lo, uint32_t hi) {
    for (int i = 0; i < c->nheld; ) {
        if (seqdiff(lo, c->held_hi[i]) <= 0 && seqdiff(c->held_lo[i], hi) <= 0) {
            if (seqdiff(c->held_lo[i], lo) < 0) lo = c->held_lo[i];
            if (seqdiff(c->held_hi[i], hi) > 0) hi = c->held_hi[i];
            tcp_held_drop(c, i);
            continue;
        }
        i++;
    }
    if (c->nheld < TCP_HELD) {
        c->held_lo[c->nheld] = lo;
        c->held_hi[c->nheld] = hi;
        c->nheld++;
    }
}

// The gap has closed up to rcv_nxt: take in every held range it now reaches.
static void tcp_held_join(tcp_conn_t *c) {
    for (int i = 0; i < c->nheld; ) {
        if (seqdiff(c->held_lo[i], c->rcv_nxt) <= 0) {
            int32_t gain = seqdiff(c->held_hi[i], c->rcv_nxt);
            if (gain > 0) {
                c->rx_len += gain;
                c->rcv_nxt = c->held_hi[i];
                tcp_bytes_in += (uint32_t)gain;
            }
            tcp_held_drop(c, i);
            i = 0;                  // rcv_nxt moved: look again from the start
            continue;
        }
        i++;
    }
}

// --- sending ----------------------------------------------------------------------

// Bytes [off, off+n) of what is queued, out of the ring into `dst`.
static void tx_copy(const tcp_conn_t *c, int off, uint8_t *dst, int n) {
    int at = (c->tx_head + off) % TCP_TX_CAP;
    int first = TCP_TX_CAP - at < n ? TCP_TX_CAP - at : n;
    nmemcpy(dst, c->tx + at, first);
    if (n > first) nmemcpy(dst + first, c->tx, n - first);
}

static void tcp_arm(tcp_conn_t *c) {
    if (!c->rtx_at) c->rtx_at = now_ms() + (uint64_t)c->rto;
}

// Send what the windows allow: the peer's, and our own estimate of the path.
// Then the FIN, once everything before it has gone.
static void tcp_push(tcp_conn_t *c) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSING) return;
    if (!c->tx) return;
    int sent = seqdiff(c->snd_nxt, c->snd_una);
    if (c->fin_sent) return;                      // all of it is out already
    int wnd = c->snd_wnd < c->cwnd ? c->snd_wnd : c->cwnd;
    while (sent < c->tx_len) {
        int room = wnd - sent;
        if (room <= 0) break;
        int n = c->tx_len - sent;
        if (n > c->mss) n = c->mss;
        if (n > room) n = room;
        tcp_seg_from(c, TCP_ACK | TCP_PSH, c->snd_nxt, NULL, sent, n);
        c->snd_nxt += (uint32_t)n;
        sent += n;
        if (seqdiff(c->snd_nxt, c->snd_max) > 0) c->snd_max = c->snd_nxt;
        tcp_arm(c);
    }
    if (c->fin_want && sent == c->tx_len) {
        c->fin_seq = c->snd_nxt;
        tcp_seg(c, TCP_FIN | TCP_ACK, c->snd_nxt, NULL, 0);
        c->snd_nxt += 1;
        c->fin_sent = 1;
        c->state = TCP_CLOSING;
        if (seqdiff(c->snd_nxt, c->snd_max) > 0) c->snd_max = c->snd_nxt;
        tcp_arm(c);
    }
}

// The oldest unacknowledged segment again, now (three duplicate
// acknowledgements say it was lost).
static void tcp_resend_first(tcp_conn_t *c) {
    if (!c->tx_len) return;
    int n = c->tx_len < c->mss ? c->tx_len : c->mss;
    tcp_seg_from(c, TCP_ACK | TCP_PSH, c->snd_una, NULL, 0, n);
}

// Queue what fits; returns how much did. The caller pumps and comes back
// for the rest.
static int tcp_queue(tcp_conn_t *c, const uint8_t *p, int len) {
    if (!c->tx || c->fin_want) return 0;
    int room = TCP_TX_CAP - c->tx_len;
    if (len > room) len = room;
    for (int i = 0; i < len; ) {
        int at = (c->tx_head + c->tx_len) % TCP_TX_CAP;
        int n = TCP_TX_CAP - at < len - i ? TCP_TX_CAP - at : len - i;
        nmemcpy(c->tx + at, p + i, n);
        c->tx_len += n;
        i += n;
    }
    tcp_push(c);
    return len;
}

static void tcp_dead(tcp_conn_t *c, int err) {
    c->state = TCP_CLOSED;
    c->remote_closed = 1;
    if (!c->err) c->err = err;
    c->rtx_at = 0;
}

// The retransmission timer ran out.
static void tcp_timeout(tcp_conn_t *c) {
    c->rtx_at = 0;
    int opening = c->state == TCP_SYN_SENT || c->state == TCP_SYN_RCVD;
    // A SYN is asked again every 600 ms, five times, and then given up on --
    // a host that is not there must not hold a page for long.
    if (++c->retries > (opening ? 5 : TCP_RETRIES)) {
        tcp_dead(c, NET_ETIMEDOUT);
        return;
    }
    if (!opening) c->rto = c->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : c->rto * 2;
    if (c->state == TCP_SYN_SENT) {
        tcp_seg(c, TCP_SYN, c->iss, NULL, 0);
        tcp_arm(c);
        return;
    }
    if (c->state == TCP_SYN_RCVD) {
        tcp_seg(c, TCP_SYN | TCP_ACK, c->iss, NULL, 0);
        tcp_arm(c);
        return;
    }
    int out = seqdiff(c->snd_nxt, c->snd_una);
    if (out <= 0 && !(c->snd_wnd == 0 && c->tx_len > 0)) return;
    // Back to the first unacknowledged byte, one segment at a time again.
    c->snd_nxt = c->snd_una;
    c->fin_sent = 0;
    if (c->state == TCP_CLOSING && !c->fin_want) c->state = TCP_ESTABLISHED;
    c->cwnd = c->mss;
    int saved = c->snd_wnd;
    if (c->snd_wnd == 0) c->snd_wnd = 1;          // a probe: is the window still shut?
    tcp_push(c);
    c->snd_wnd = saved;
    tcp_arm(c);
}

// --- receiving --------------------------------------------------------------------

static int tcp_mss_opt(const uint8_t *seg, int hlen) {
    for (int i = (int)sizeof(tcp_hdr_t); i + 1 < hlen; ) {
        uint8_t k = seg[i];
        if (k == 0) break;
        if (k == 1) { i++; continue; }
        uint8_t l = seg[i + 1];
        if (l < 2 || i + l > hlen) break;
        if (k == 2 && l == 4) return seg[i + 2] << 8 | seg[i + 3];
        i += l;
    }
    return 536;                                   // what is assumed without one
}

static int tcp_alloc(void);
static void tcp_release(tcp_conn_t *c);

// A handle given back whose connection has ended both ways: our FIN
// acknowledged and theirs arrived (or it is dead).
static int orphan_done(const tcp_conn_t *c) {
    if (!c->orphan) return 0;
    if (c->state == TCP_CLOSED) return 1;
    return c->fin_sent && seqdiff(c->snd_una, c->fin_seq) > 0 && c->remote_closed;
}

// A SYN for a port somebody listens on: a new connection, half open.
static void tcp_passive(int L, uint32_t srcip, uint32_t dstip, const tcp_hdr_t *t,
                        const uint8_t *seg, int hlen) {
    int pending = lsn[L].qn;
    for (int i = 0; i < TCP_CONNS; i++)
        if (conns[i].state == TCP_SYN_RCVD && conns[i].listener == L) pending++;
    int h = pending < lsn[L].backlog ? tcp_alloc() : -1;
    if (h < 0) {
        tcp_reset_reply(srcip, dstip, ntohs(t->src_port), ntohs(t->dst_port), 0, ntohl(t->seq) + 1, 1);
        return;
    }
    tcp_conn_t *c = &conns[h];
    c->rx = kmalloc((size_t)lsn[L].rx_cap);
    c->tx = kmalloc(TCP_TX_CAP);
    if (!c->rx || !c->tx) { tcp_release(c); return; }
    c->rx_cap = lsn[L].rx_cap;
    c->remote_ip = srcip;
    c->local_ip = dstip;
    c->remote_port = ntohs(t->src_port);
    c->local_port = ntohs(t->dst_port);
    c->rcv_nxt = ntohl(t->seq) + 1;
    c->iss = (uint32_t)(now_us() * 2654435761u);
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->iss + 1;
    c->mss = tcp_mss_opt(seg, hlen);
    if (c->mss > TCP_MSS) c->mss = TCP_MSS;
    c->snd_wnd = ntohs(t->window);
    c->cwnd = 10 * c->mss;
    c->rto = TCP_RTO_SYN;
    c->listener = L;
    c->state = TCP_SYN_RCVD;
    tcp_seg(c, TCP_SYN | TCP_ACK, c->iss, NULL, 0);
    tcp_arm(c);
}

static void tcp_input(uint32_t srcip, uint32_t dstip, const uint8_t *seg, int len) {
    if (len < (int)sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *t = (const tcp_hdr_t *)seg;

    uint8_t flags = t->flags;
    uint32_t seq = ntohl(t->seq);
    uint32_t ack = ntohl(t->ack);
    int hlen = (t->data_off >> 4) * 4;
    if (hlen < (int)sizeof(tcp_hdr_t) || hlen > len) return;
    int dlen = len - hlen;
    const uint8_t *data = seg + hlen;

    tcp_conn_t *c = tcp_lookup(srcip, ntohs(t->src_port), ntohs(t->dst_port));
    if (!c) {
        if (flags & TCP_RST) return;
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            for (int L = 0; L < TCP_LISTENERS; L++)
                if (lsn[L].used && lsn[L].port == ntohs(t->dst_port) &&
                    (!lsn[L].ip || lsn[L].ip == dstip)) {
                    tcp_passive(L, srcip, dstip, t, seg, hlen);
                    return;
                }
        }
        // Nobody here: say so, rather than let the peer wait it out.
        if (flags & TCP_ACK)
            tcp_reset_reply(srcip, dstip, ntohs(t->src_port), ntohs(t->dst_port), ack, 0, 0);
        else
            tcp_reset_reply(srcip, dstip, ntohs(t->src_port), ntohs(t->dst_port), 0,
                            seq + (uint32_t)dlen + ((flags & TCP_SYN) ? 1u : 0u), 1);
        return;
    }

    if (flags & TCP_RST) {
        tcp_dead(c, c->state == TCP_SYN_SENT ? NET_ECONNREFUSED : NET_ECONNRESET);
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK) && ack == c->iss + 1) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->snd_nxt = c->snd_max = ack;
            c->mss = tcp_mss_opt(seg, hlen);
            if (c->mss > TCP_MSS) c->mss = TCP_MSS;
            c->snd_wnd = ntohs(t->window);
            c->cwnd = 10 * c->mss;
            c->rto = TCP_RTO_INIT;
            c->retries = 0;
            c->rtx_at = 0;
            c->state = TCP_ESTABLISHED;
            tcp_out(c, TCP_ACK, NULL, 0);
            tcp_push(c);
        }
        return;
    }

    if (c->state == TCP_SYN_RCVD) {
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {    // our SYN-ACK was lost
            tcp_seg(c, TCP_SYN | TCP_ACK, c->iss, NULL, 0);
            return;
        }
        if (!(flags & TCP_ACK) || ack != c->iss + 1) return;
        c->snd_una = ack;
        c->rto = TCP_RTO_INIT;
        c->retries = 0;
        c->rtx_at = 0;
        c->state = TCP_ESTABLISHED;
        int L = c->listener;
        if (L >= 0 && lsn[L].used && lsn[L].qn < TCP_BACKLOG)
            lsn[L].q[lsn[L].qn++] = (int)(c - conns);
        // and on, for any data it carries
    }

    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSING) return;

    // What the peer has acknowledged of ours.
    if (flags & TCP_ACK) {
        int32_t acked = seqdiff(ack, c->snd_una);
        if (acked > 0 && seqdiff(ack, c->snd_max) <= 0) {
            int data_acked = acked;
            if (c->fin_sent && seqdiff(ack, c->fin_seq) > 0) data_acked -= 1;
            if (data_acked > c->tx_len) data_acked = c->tx_len;
            c->tx_head = (c->tx_head + data_acked) % TCP_TX_CAP;
            c->tx_len -= data_acked;
            c->snd_una = ack;
            if (seqdiff(c->snd_nxt, ack) < 0) c->snd_nxt = ack;   // acknowledged past a resend
            c->retries = 0;
            c->rto = TCP_RTO_INIT;
            c->dupacks = 0;
            if (c->cwnd < 64 * 1024) c->cwnd += c->mss;
            c->rtx_at = 0;
            if (seqdiff(c->snd_nxt, c->snd_una) > 0) tcp_arm(c);
            c->snd_wnd = ntohs(t->window);
        } else if (acked == 0) {
            int w = ntohs(t->window);
            if (dlen == 0 && !(flags & (TCP_SYN | TCP_FIN)) && w == c->snd_wnd &&
                seqdiff(c->snd_nxt, c->snd_una) > 0 && ++c->dupacks == 3)
                tcp_resend_first(c);
            c->snd_wnd = w;
        }
    }

    uint32_t fin_seq = seq + (uint32_t)dlen;   // where a FIN here would sit
    if (dlen > 0 && !c->remote_closed) {
        // Drop the part we already have: a retransmission that overlaps.
        int32_t ahead = seqdiff(seq, c->rcv_nxt);
        if (ahead < 0) {
            int dup = -ahead;
            if (dup >= dlen) dlen = 0;
            else { data += dup; dlen -= dup; seq = c->rcv_nxt; ahead = 0; }
        }
        if (dlen > 0) {
            // Slide the unread bytes back to the front only when the
            // room behind them has run out -- not on every read.
            if (tcp_rx_space(c) < ahead + dlen) tcp_compact(c);
            int space = tcp_rx_space(c);
            int take = ahead >= space ? 0 : (dlen < space - ahead ? dlen : space - ahead);
            if (take > 0) {
                nmemcpy(c->rx + c->rx_head + c->rx_len + ahead, data, take);
                if (ahead == 0) {
                    c->rx_len += take;
                    c->rcv_nxt += (uint32_t)take;   // only what was kept
                    tcp_bytes_in += (uint32_t)take;
                    tcp_held_join(c);
                } else {
                    tcp_hold(c, seq, seq + (uint32_t)take);
                }
            }
        }
        // Every data segment is answered, in order or not: past a gap
        // the answer repeats the last acknowledgement, which is what
        // tells the sender to resend the missing piece now rather than
        // when its timer runs out.
        tcp_out(c, TCP_ACK, NULL, 0);
        // A connection nobody holds any more throws what arrives away.
        if (c->orphan) { c->rx_head = 0; c->rx_len = 0; }
    } else if (dlen > 0) {
        tcp_out(c, TCP_ACK, NULL, 0);
    }
    // A FIN counts only once everything before it has arrived: one that
    // overtakes a lost segment would end the stream short.
    if ((flags & TCP_FIN) && fin_seq == c->rcv_nxt && !c->remote_closed) {
        c->rcv_nxt += 1;
        c->remote_closed = 1;
        tcp_out(c, TCP_ACK, NULL, 0);
    } else if ((flags & TCP_FIN) && c->remote_closed && seqdiff(fin_seq + 1, c->rcv_nxt) <= 0) {
        tcp_out(c, TCP_ACK, NULL, 0);             // our ACK of it was lost
    }
    tcp_push(c);
    // Its last segment: free it now rather than at the next timer pass --
    // over the loopback a whole connection can open and close inside one.
    if (orphan_done(c)) tcp_release(c);
}

// A local port nobody else here is using. They are ours to choose and the
// peer echoes them back, so this is also what tells two connections apart.
static uint16_t tcp_pick_port(void) {
    static uint16_t roll;
    if (roll == 0) roll = (uint16_t)(now_ms() & 0x1FFF);
    for (int tries = 0; tries < 4096; tries++) {
        uint16_t p = (uint16_t)(40000 + (roll++ % 20000));
        int taken = 0;
        for (int i = 0; i < TCP_CONNS; i++)
            if ((conns[i].state != TCP_CLOSED || conns[i].rx) && conns[i].local_port == p)
                taken = 1;
        for (int i = 0; i < TCP_LISTENERS; i++)
            if (lsn[i].used && lsn[i].port == p) taken = 1;
        if (!taken) return p;
    }
    return 0;
}

static int tcp_alloc(void) {
    for (int i = 0; i < TCP_CONNS; i++)
        if (orphan_done(&conns[i])) tcp_release(&conns[i]);
    for (int i = 0; i < TCP_CONNS; i++)
        if (conns[i].state == TCP_CLOSED && conns[i].rx == NULL && conns[i].tx == NULL) {
            nmemset(&conns[i], 0, sizeof conns[i]);
            conns[i].listener = -1;
            conns[i].mss = 536;
            conns[i].rto = TCP_RTO_INIT;
            return i;
        }
    return -1;
}

static void tcp_release(tcp_conn_t *c) {
    if (c->rx) kfree(c->rx);
    if (c->tx) kfree(c->tx);
    nmemset(c, 0, sizeof(*c));
    c->listener = -1;
}

// Begin a connection: the SYN goes out, and the answer comes in on its own.
// Returns the handle, or -1 (none free, no memory, no port).
static int tcp_start(uint32_t ip, uint16_t port, int rx_cap, uint16_t lport) {
    int h = tcp_alloc();
    if (h < 0) return -1;
    if (rx_cap < 8192) rx_cap = 8192;
    tcp_conn_t *c = &conns[h];
    c->rx = kmalloc((size_t)rx_cap);
    c->tx = kmalloc(TCP_TX_CAP);
    if (!c->rx || !c->tx) { tcp_release(c); return -1; }
    c->rx_cap = rx_cap;
    c->remote_ip = ip;
    c->local_ip = is_loop(ip) ? ip : our_ip;
    c->remote_port = port;
    c->local_port = lport ? lport : tcp_pick_port();
    if (c->local_port == 0) { tcp_release(c); return -1; }
    c->iss = (uint32_t)(now_us() * 2654435761u);
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->iss + 1;          // the SYN takes one
    c->rto = TCP_RTO_SYN;
    c->state = TCP_SYN_SENT;
    tcp_seg(c, TCP_SYN, c->iss, NULL, 0);
    tcp_arm(c);
    return h;
}

// Open a connection and wait for it. Returns the handle, or -1. `rx_cap`
// is how much of the stream may sit unread before the peer is told to stop:
// the plain-HTTP path accumulates a whole response and wants a great deal,
// TLS drains every record as it lands and wants very little.
static int tcp_open(uint32_t ip, uint16_t port, int rx_cap) {
    int h = tcp_start(ip, port, rx_cap, 0);
    if (h < 0) return -1;
    tcp_conn_t *c = &conns[h];
    while (c->state == TCP_SYN_SENT) net_poll();  // the timer gives up on it
    if (c->state == TCP_ESTABLISHED) { af_note_socket(h); return h; }
    tcp_release(c);
    return -1;
}

// Queue all of it, pumping while the buffer is full.
static void tcp_send_data(tcp_conn_t *c, const void *data, int len) {
    const uint8_t *p = data;
    uint64_t give_up = now_ms() + 30000;
    while (len > 0 && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSING) && now_ms() < give_up) {
        int n = tcp_queue(c, p, len);
        p += n; len -= n;
        if (len > 0) net_poll();
    }
}

// Shut our side: the FIN follows whatever is still queued.
static void tcp_close(tcp_conn_t *c) {
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSING) {
        c->fin_want = 1;
        tcp_push(c);
    } else if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RCVD) {
        tcp_dead(c, 0);
    }
}

// Run every connection's timers: retransmission, and letting go of closed
// handles whose FIN has landed (or never will).
static int tcp_timers_busy;
static uint64_t tcp_timers_last;
static void tcp_timers(void) {
    if (tcp_timers_busy) return;
    uint64_t now = now_ms();
    if (now == tcp_timers_last) return;
    tcp_timers_last = now;
    tcp_timers_busy = 1;
    for (int i = 0; i < TCP_CONNS; i++) {
        tcp_conn_t *c = &conns[i];
        if (c->state == TCP_CLOSED && !c->rx) continue;
        if (c->rtx_at && now >= c->rtx_at && c->state != TCP_CLOSED) tcp_timeout(c);
        if (c->orphan) {
            // Done when our FIN is acknowledged and theirs has come; past
            // the linger, done anyway -- quietly if ours at least landed.
            int fin_acked = c->fin_sent && seqdiff(c->snd_una, c->fin_seq) > 0;
            int done = orphan_done(c);
            if (done || now >= c->orphan_until) {
                if (!done && !fin_acked)
                    tcp_seg(c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0);
                tcp_release(c);
            }
        }
    }
    tcp_timers_busy = 0;
}

// --- the stream interface used by tls.c, net_http_get and the sockets --------------
//
// TLS needs to read a length-prefixed record, then the next one, which is not
// something "drain everything then look at it" can do. These expose one
// connection as a byte stream: fill waits for bytes to arrive, peek looks at
// them, consume drops the ones that have been used. Everything stays inside
// net.c because the connection state does.
//
// Every one of them takes a handle. There is no current connection and no
// default, deliberately -- an implicit one is how this came to be single
// -connection in the first place.

int net_tcp_open(uint32_t ip, uint16_t port, int rx_cap) {
    return tcp_open(ip, port, rx_cap);
}

int net_tcp_start(uint32_t ip, uint16_t port, int rx_cap) {
    return tcp_start(ip, port, rx_cap, 0);
}

int net_tcp_state(int h) {
    tcp_conn_t *c = conn_of(h);
    if (!c) return NET_TCP_DEAD;
    if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RCVD) return NET_TCP_OPENING;
    if (c->state == TCP_CLOSED) return NET_TCP_DEAD;
    return NET_TCP_OPEN;
}

int net_tcp_error(int h) {
    tcp_conn_t *c = conn_of(h);
    return c ? c->err : 0;
}

int net_tcp_write(int h, const void *data, int len) {
    af_note_socket(h);
    tcp_conn_t *c = conn_of(h);
    if (!c || c->state != TCP_ESTABLISHED) return -1;
    tcp_send_data(c, data, len);
    return len;
}

// As much as fits now, without waiting; -1 if the connection cannot send.
int net_tcp_send_some(int h, const void *data, int len) {
    tcp_conn_t *c = conn_of(h);
    if (!c || c->state != TCP_ESTABLISHED || c->fin_want) return -1;
    return tcp_queue(c, (const uint8_t *)data, len);
}

// Room in the send buffer.
int net_tcp_send_room(int h) {
    tcp_conn_t *c = conn_of(h);
    if (!c || !c->tx) return 0;
    return TCP_TX_CAP - c->tx_len;
}

// Bytes queued and not yet acknowledged.
int net_tcp_unacked(int h) {
    tcp_conn_t *c = conn_of(h);
    return c ? c->tx_len : 0;
}

int net_tcp_avail(int h) {
    tcp_conn_t *c = conn_of(h);
    return c ? c->rx_len : 0;
}

const uint8_t *net_tcp_peek(int h) {
    tcp_conn_t *c = conn_of(h);
    return c ? c->rx + c->rx_head : NULL;
}

int net_tcp_closed(int h) {
    tcp_conn_t *c = conn_of(h);
    return !c || c->remote_closed || c->state == TCP_CLOSED;
}

void net_tcp_consume(int h, int n) {
    tcp_conn_t *c = conn_of(h);
    if (!c || n <= 0) return;
    if (n > c->rx_len) n = c->rx_len;
    c->rx_head += n;
    c->rx_len  -= n;
    // Empty: start from the front again -- unless data held past a gap is
    // sitting further along, addressed from where the unread bytes end.
    if (c->rx_len == 0 && c->nheld == 0) c->rx_head = 0;

    // Tell the sender about room it cannot see yet. It learns the window only
    // from our acknowledgements, and those go out when data arrives. So once
    // a fast sender has filled the buffer and the window has shrunk to a few
    // hundred bytes, nothing more arrives, nothing goes back, and the sender
    // sits out its persist timer while the reader has long since made room.
    // Measured on a kept connection to GitHub's CDN: a 624 KB file whose
    // last two kilobytes came five seconds after the rest. A window grown by
    // two segments is worth saying so (RFC 1122, 4.2.3.3).
    if ((c->state == TCP_ESTABLISHED || c->state == TCP_CLOSING) && !c->remote_closed) {
        int win = c->rx_cap - c->rx_len;          // as tcp_seg reckons it
        if (win > 65535) win = 65535;
        if (win - c->adv_win >= 2 * 1460) tcp_out(c, TCP_ACK, NULL, 0);
    }
}

int net_tcp_fill(int h, int want, uint32_t timeout_ms) {
    af_note_socket(h);
    tcp_conn_t *c = conn_of(h);
    if (!c) return 0;
    uint64_t deadline = now_ms() + timeout_ms;
    while (c->rx_len < want) {
        if (c->remote_closed || c->state == TCP_CLOSED) break;
        if (now_ms() >= deadline) break;
        net_poll();
    }
    return c->rx_len;
}

// Send the FIN but keep whatever has already arrived readable.
void net_tcp_shutdown(int h) {
    tcp_conn_t *c = conn_of(h);
    if (c) tcp_close(c);
}

// Give the handle back. After this it is somebody else's -- though the
// connection itself may stay a few seconds more, until its last bytes and
// its FIN have been acknowledged.
void net_tcp_release(int h) {
    tcp_conn_t *c = conn_of(h);
    if (!c) return;
    tcp_close(c);
    if (c->state == TCP_CLOSING || c->state == TCP_ESTABLISHED) {
        c->orphan = 1;
        c->orphan_until = now_ms() + TCP_LINGER_MS;
        c->rx_head = c->rx_len = 0;
        return;
    }
    tcp_release(c);
}

// Throw it away now: a reset to the peer, the handle free at once.
void net_tcp_abort(int h) {
    tcp_conn_t *c = conn_of(h);
    if (!c) return;
    if (c->state != TCP_CLOSED) tcp_seg(c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0);
    tcp_release(c);
}

int net_tcp_open_count(void) {
    int n = 0;
    for (int i = 0; i < TCP_CONNS; i++)
        if (conns[i].rx) n++;
    return n;
}

void net_tcp_names(int h, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport) {
    tcp_conn_t *c = conn_of(h);
    if (!c) return;
    if (lip) *lip = c->local_ip ? c->local_ip : our_ip;
    if (lport) *lport = c->local_port;
    if (rip) *rip = c->remote_ip;
    if (rport) *rport = c->remote_port;
}

// --- listening ----------------------------------------------------------------------

int net_tcp_listen(uint32_t ip, uint16_t port, int backlog, int rx_cap) {
    if (!port) return -NET_EINVAL;
    for (int i = 0; i < TCP_LISTENERS; i++)
        if (lsn[i].used && lsn[i].port == port) return -NET_EADDRINUSE;
    for (int i = 0; i < TCP_LISTENERS; i++) {
        if (lsn[i].used) continue;
        lsn[i].used = 1;
        lsn[i].port = port;
        lsn[i].ip = ip;
        lsn[i].backlog = backlog < 1 ? 1 : (backlog > TCP_BACKLOG ? TCP_BACKLOG : backlog);
        lsn[i].rx_cap = rx_cap < 8192 ? 8192 : rx_cap;
        lsn[i].qn = 0;
        return i;
    }
    return -NET_ENOBUFS;
}

int net_tcp_pending(int L) {
    if (L < 0 || L >= TCP_LISTENERS || !lsn[L].used) return 0;
    return lsn[L].qn;
}

// The oldest connection that came in and has finished its handshake, or -1.
int net_tcp_accept(int L) {
    if (L < 0 || L >= TCP_LISTENERS || !lsn[L].used || !lsn[L].qn) return -1;
    int h = lsn[L].q[0];
    for (int i = 1; i < lsn[L].qn; i++) lsn[L].q[i - 1] = lsn[L].q[i];
    lsn[L].qn--;
    conns[h].listener = -1;
    return h;
}

void net_tcp_unlisten(int L) {
    if (L < 0 || L >= TCP_LISTENERS || !lsn[L].used) return;
    for (int i = 0; i < lsn[L].qn; i++) net_tcp_abort(lsn[L].q[i]);
    for (int i = 0; i < TCP_CONNS; i++)
        if (conns[i].state == TCP_SYN_RCVD && conns[i].listener == L) net_tcp_abort(i);
    nmemset(&lsn[L], 0, sizeof lsn[L]);
}

// --- UDP for programs ---------------------------------------------------------------
//
// A port, and a queue of what arrived on it -- several datagrams, each with
// where it came from. (The boxes above are for the kernel's own one-question
// exchanges: DNS, NTP.)

#define UDP_SOCKS 16
#define UDP_QLEN  16
#define UDP_MAX   1472                // one Ethernet frame; there is no fragmenting

typedef struct { uint32_t ip; uint16_t port; uint16_t len; uint8_t data[UDP_MAX]; } udp_dgram_t;
static struct {
    int          used;
    uint16_t     port;
    uint32_t     ip;                  // bound to this address of ours, 0 = any
    int          head, n;
    udp_dgram_t *q;
} usk[UDP_SOCKS];

static int udp_port_taken(uint16_t p) {
    for (int i = 0; i < UDP_SOCKS; i++) if (usk[i].used && usk[i].port == p) return 1;
    return p == 68 || p == 53;
}

int net_udp_open(uint32_t ip, uint16_t port) {
    if (port && udp_port_taken(port)) return -NET_EADDRINUSE;
    if (!port) {
        static uint16_t roll;
        for (int t = 0; t < 7000 && !port; t++) {
            uint16_t p = (uint16_t)(33000 + (roll++ % 7000));
            if (!udp_port_taken(p)) port = p;
        }
        if (!port) return -NET_EADDRINUSE;
    }
    for (int i = 0; i < UDP_SOCKS; i++) {
        if (usk[i].used) continue;
        usk[i].q = kmalloc(sizeof(udp_dgram_t) * UDP_QLEN);
        if (!usk[i].q) return -NET_ENOBUFS;
        usk[i].used = 1;
        usk[i].port = port;
        usk[i].ip = ip;
        usk[i].head = usk[i].n = 0;
        return i;
    }
    return -NET_ENOBUFS;
}

uint16_t net_udp_port(int u) { return u >= 0 && u < UDP_SOCKS && usk[u].used ? usk[u].port : 0; }

void net_udp_close(int u) {
    if (u < 0 || u >= UDP_SOCKS || !usk[u].used) return;
    kfree(usk[u].q);
    nmemset(&usk[u], 0, sizeof usk[u]);
}

int net_udp_pending(int u) {
    if (u < 0 || u >= UDP_SOCKS || !usk[u].used) return 0;
    return usk[u].n;
}

static int udp_deliver(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                       const uint8_t *data, int len) {
    for (int i = 0; i < UDP_SOCKS; i++) {
        if (!usk[i].used || usk[i].port != dport) continue;
        if (usk[i].ip && usk[i].ip != dst && dst != 0xFFFFFFFF) continue;
        if (usk[i].n == UDP_QLEN) return 1;           // full: this one is lost, as UDP may
        udp_dgram_t *d = &usk[i].q[(usk[i].head + usk[i].n) % UDP_QLEN];
        if (len > UDP_MAX) len = UDP_MAX;
        d->ip = src;
        d->port = sport;
        d->len = (uint16_t)len;
        nmemcpy(d->data, data, len);
        usk[i].n++;
        return 1;
    }
    return 0;
}

// The next datagram: copied into buf (cut to `len`), its sender, its whole
// length returned; -1 when there is none.
int net_udp_recv(int u, void *buf, int len, uint32_t *ip, uint16_t *port, int peek) {
    if (u < 0 || u >= UDP_SOCKS || !usk[u].used || !usk[u].n) return -1;
    udp_dgram_t *d = &usk[u].q[usk[u].head];
    int n = d->len < len ? d->len : len;
    nmemcpy(buf, d->data, n);
    if (ip) *ip = d->ip;
    if (port) *port = d->port;
    int whole = d->len;
    if (!peek) { usk[u].head = (usk[u].head + 1) % UDP_QLEN; usk[u].n--; }
    return whole;
}

static int udp_send(uint32_t dstip, uint16_t sport, uint16_t dport, const void *data, int len);

int net_udp_send(int u, uint32_t ip, uint16_t port, const void *data, int len) {
    if (u < 0 || u >= UDP_SOCKS || !usk[u].used) return -NET_EINVAL;
    if (len > UDP_MAX) return -NET_EMSGSIZE;
    if (!is_loop(ip) && !up) return -NET_ENETDOWN;
    if (udp_send(ip, usk[u].port, port, data, len) != 0) return -NET_EHOSTUNREACH;
    return len;
}

// Is anything open that needs the network looked after in the background?
int net_active(void) {
    for (int i = 0; i < TCP_CONNS; i++) if (conns[i].rx) return 1;
    for (int i = 0; i < TCP_LISTENERS; i++) if (lsn[i].used) return 1;
    for (int i = 0; i < UDP_SOCKS; i++) if (usk[i].used) return 1;
    return 0;
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

// What came back for question `txid`: 1 with the first address in *ip; 0 if
// it is the answer and there is no address in it -- no such name; -2 if the
// server could not answer just now and asking again may help; -1 if it is not
// the answer to this question at all.
static int dns_answer(const uint8_t *r, int rl, uint16_t txid, uint32_t *ip) {
    if (rl < 12 || ntohs(*(const uint16_t *)r) != txid) return -1;
    if (!(r[2] & 0x80)) return -1;                   // a question, not an answer
    int rcode = r[3] & 0x0F;
    if (rcode == 3) return 0;                        // no such name
    if (rcode != 0) return -2;                       // server failure and the like
    uint16_t ancount = ntohs(*(const uint16_t *)(r + 6));
    int off = 12;
    while (off < rl && r[off]) off += r[off] + 1;    // the question's name
    off += 1 + 4;                                    // its end, QTYPE, QCLASS
    for (int a = 0; a < ancount && off < rl; a++) {
        if ((r[off] & 0xC0) == 0xC0) off += 2;       // the name, as a pointer
        else { while (off < rl && r[off]) off += r[off] + 1; off += 1; }
        if (off + 10 > rl) break;
        uint16_t atype = ntohs(*(const uint16_t *)(r + off));
        uint16_t rdlen = ntohs(*(const uint16_t *)(r + off + 8));
        off += 10;
        if (off + rdlen > rl) break;
        if (atype == 1 && rdlen == 4) {              // A: the address
            uint32_t a4;
            nmemcpy(&a4, r + off, 4);
            *ip = ntohl(a4);
            return 1;
        }
        off += rdlen;                                // a CNAME on the way to it
    }
    return 0;
}

// A source port and a transaction id that do not repeat while anything is
// still waiting on the last ones. Several lookups may now be outstanding at
// once, and the only thing telling their answers apart is this pair.
static uint16_t dns_seq;

static int dns_query(const char *name, uint32_t *ip) {

    uint8_t q[512];
    uint16_t tick = ++dns_seq;
    uint16_t txid = (uint16_t)((now_ms() & 0xFFFF) ^ 0xA5A5) + tick;
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

    uint16_t sport = (uint16_t)(50000 + (tick % 8000));
    udp_box_t *box = udp_box_open(sport);
    if (!box) return 0;

    int found = 0;
    for (int tries = 0; tries < 3; tries++) {
        box->have = 0;
        if (udp_send(dns_ip, sport, 53, q, n) != 0) break;
        int verdict = -1;
        uint64_t deadline = now_ms() + 1200;
        while (verdict == -1 && now_ms() < deadline) {
            net_poll();
            if (!box->have) continue;
            if (box->sip == dns_ip) verdict = dns_answer(box->data, box->len, txid, ip);
            box->have = 0;                       // room for the next one
        }
        if (verdict == 1) { found = 1; break; }
        if (verdict == 0) break;                 // an answer, and it is no
        // Silence, or a server that could not say: ask again.
    }
    udp_box_close(box);
    return found;
}

// ==========================================================================
//  NTP
// ==========================================================================
//
// 48 bytes out, 48 back. The server copies our transmit stamp into its
// "origin" field, which is how an answer is told from a stale or forged one;
// what we send there need not be a time at all, only something unguessable
// enough, so it is the counter. The answer carries when the server got the
// request (T2) and when it sent the reply (T3); the time it spent in between
// is not network, so the one-way trip is (round trip - (T3 - T2)) / 2, and
// the time at arrival T3 plus that.

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
// An NTP stamp (seconds since 1900, 32.32) in milliseconds since 1970.
static int64_t ntp_ms(const uint8_t *p) {
    int64_t s = (int64_t)get_be32(p) - 2208988800LL;
    return s * 1000 + (int64_t)(((uint64_t)get_be32(p + 4) * 1000) >> 32);
}

int net_ntp(uint32_t ip, int tries, int64_t *utc_ms, uint64_t *at_us, uint32_t *rtt_ms) {
    if (!up) return 0;
    static uint16_t seq;
    uint16_t sport = (uint16_t)(40000 + (++seq % 4000));
    udp_box_t *box = udp_box_open(sport);
    if (!box) return 0;

    int ok = 0;
    for (int t = 0; t < tries && !ok; t++) {
        uint8_t q[48];
        nmemset(q, 0, sizeof q);
        q[0] = 0x23;                              // no leap warning, version 4, client
        uint64_t t1 = now_us();
        uint32_t tag_hi = (uint32_t)(t1 >> 32) ^ 0x6E54C0DEu, tag_lo = (uint32_t)t1 ^ (uint32_t)seq;
        put_be32(q + 40, tag_hi);
        put_be32(q + 44, tag_lo);
        box->have = 0;
        if (udp_send(ip, sport, 123, q, sizeof q) != 0) break;
        uint64_t deadline = now_ms() + 1000;
        while (!ok && now_ms() < deadline) {
            net_poll();
            if (!box->have) continue;
            uint64_t t4 = now_us();
            const uint8_t *r = box->data;
            int good = box->sip == ip && box->len >= 48
                       && (r[0] & 7) == 4                 // a server's answer
                       && (r[0] >> 6) != 3                // its clock is set
                       && r[1] >= 1 && r[1] <= 15         // stratum: not a "kiss of death"
                       && get_be32(r + 24) == tag_hi && get_be32(r + 28) == tag_lo;
            if (good) {
                int64_t t2 = ntp_ms(r + 32), t3 = ntp_ms(r + 40);
                int64_t rtt = (int64_t)(t4 - t1) / 1000 - (t3 - t2);
                if (rtt < 0) rtt = 0;
                *utc_ms = t3 + rtt / 2;
                *at_us = t4;
                *rtt_ms = (uint32_t)rtt;
                ok = 1;
            }
            box->have = 0;
        }
    }
    udp_box_close(box);
    return ok;
}

// ==========================================================================
//  fetching on its own stack
// ==========================================================================
//
// A fetch is seconds of waiting. Done inside one system call it stopped the
// machine for all of them, so each one runs as a coroutine on its own stack
// and hands the processor back every few milliseconds.
//
// There are six of them now. A page is a document and then twenty files, and
// with one slot those twenty went in single file -- twenty-one round trips
// end to end, most of the time spent with nothing on the wire at all. Each
// slot has its own TCP connection, TLS session and stack, and the twenty
// files overlap. Six is what browsers settled on per host; four was enough
// only while the browser itself was the slow part.
//
// What makes this safe without a single lock is that the tasks are
// cooperative: a slot only ever gives up the processor inside net_poll(),
// and only because the slice ran out. Between resuming a slot and it coming
// back, nothing else in the kernel runs. That is also why tls_use() is called
// exactly where it is -- one instruction before the resume, which is the only
// moment at which "the session in play" can be set and stay true.

enum { AF_IDLE, AF_RUN, AF_DONE };

// How long a fetch may hold the processor before handing it back. Short
// enough that a window redrawing at 60Hz does not visibly stutter.
#define AF_SLICE_US 4000
#define AF_STACK    (64 * 1024)
#define AF_SLOTS    TLS_SESSIONS     // a slot is a TLS session and a stack
#define AF_BUF_MAX  (1024 * 1024)

// Set while a fetch task is the one holding the processor, and the moment it
// has to hand it back. Only one slot runs at a time, so one pair does.
static int      af_running;
static uint64_t af_slice_end;

typedef struct {
    int      state;
    char     host[160];
    char     path[1024];
    uint16_t port;
    int      tls, raw;
    int      result;            // bytes, or -1
    char     body[4096];        // a form being posted; empty for a GET
    int      blen;
    char     xhdr[2048];        // headers the browser wrote, Cookie among them
    task_t  *task;
    uint8_t *buf;               // the answer; a megabyte, on first use
    int      bytes0;            // tcp_bytes_in when this fetch began
    int      progress;
    int      sock;              // the connection it opened last, or -1
} af_slot_t;

static af_slot_t afs[AF_SLOTS];

// The slot whose coroutine is running right now, so that a connection it
// opens can be attributed to it. -1 when the processor is not inside one.
static int af_current = -1;

// Which slot the task about to be started for the first time belongs to.
// A task entry function takes no arguments, and each task has its own stack,
// so it reads this once on the way in and keeps it in a local from then on.
static int af_spawning;

static void af_entry(void) {
    const int me = af_spawning;
    af_slot_t *a = &afs[me];
    for (;;) {
        uint32_t ip;
        int r = -1;
        if (net_resolve(a->host, &ip)) {
            r = a->tls
              ? tls_https_get(a->host, ip, a->port, a->path,
                              (char *)a->buf, AF_BUF_MAX, a->raw,
                              a->blen ? a->body : 0, a->blen,
                              a->xhdr[0] ? a->xhdr : 0)
              : net_http_get(a->host, ip, a->port, a->path,
                             (char *)a->buf, AF_BUF_MAX, a->raw,
                             a->blen ? a->body : 0, a->blen,
                             a->xhdr[0] ? a->xhdr : 0);
        }
        a->result = r;
        a->state = AF_DONE;
        // Nothing more to do until somebody sets up the next one.
        task_yield_back();
    }
}

static void af_note_socket(int h) {
    if (af_current >= 0) afs[af_current].sock = h;
}

int net_fetch_slots(void) { return AF_SLOTS; }

int net_fetch_busy(void) {
    for (int i = 0; i < AF_SLOTS; i++)
        if (afs[i].state == AF_RUN) return 1;
    return 0;
}

static void af_copy(char *dst, int cap, const char *src) {
    int i = 0;
    if (src) while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// Returns the slot this fetch was given, or -1 if all of them are busy or
// the memory for one could not be had.
int net_fetch_start(const char *host, const char *path, uint16_t port,
                    int tls, int keep_headers, const char *body, int blen,
                    const char *xhdr, int xhdrlen) {
    // Which idle slot. Each keeps the connection its last https request
    // used, so one already open to this host skips the TCP and TLS handshakes
    // -- two round trips, which on a phone is most of a small file. Failing
    // that, one holding nothing, so that no connection another request could
    // have reused is closed for this one. Plain http keeps no connection and
    // takes the emptiest slot, leaving the others to whoever can use them.
    int slot = -1, best = -1;
    for (int i = 0; i < AF_SLOTS; i++) {
        if (afs[i].state != AF_IDLE) continue;
        int fit = tls_pool_suits(i, host, port);
        if (!tls) fit = fit == 1 ? 2 : 0;
        if (fit > best) { best = fit; slot = i; }
    }
    if (slot < 0) return -1;

    af_slot_t *a = &afs[slot];

    // The answer buffer and the stack are made on first use and then kept:
    // most of the time one slot does all the work, and four megabytes for
    // the other three would be four megabytes wasted.
    if (!a->buf) {
        a->buf = kmalloc(AF_BUF_MAX);
        if (!a->buf) return -1;
    }
    if (!a->task) {
        af_spawning = slot;
        a->task = task_create_coroutine("fetch", af_entry, AF_STACK);
        if (!a->task) return -1;
    }

    af_copy(a->host, (int)sizeof a->host, host);
    af_copy(a->path, (int)sizeof a->path, path);
    a->port = port;
    a->tls = tls;
    a->raw = keep_headers;

    a->xhdr[0] = 0;
    if (xhdr && xhdrlen > 0) {
        if (xhdrlen > (int)sizeof a->xhdr - 1) xhdrlen = (int)sizeof a->xhdr - 1;
        for (int k = 0; k < xhdrlen; k++) a->xhdr[k] = xhdr[k];
        a->xhdr[xhdrlen] = 0;
    }
    a->blen = 0;
    if (body && blen > 0) {
        if (blen > (int)sizeof a->body) blen = (int)sizeof a->body;
        for (int k = 0; k < blen; k++) a->body[k] = body[k];
        a->blen = blen;
    }

    a->result = -1;
    a->progress = 0;
    a->sock = -1;
    a->bytes0 = (int)tcp_bytes_in;
    a->state = AF_RUN;
    return slot;
}

int net_fetch_poll(int slot, int *progress) {
    if (slot < 0 || slot >= AF_SLOTS) return -1;
    af_slot_t *a = &afs[slot];
    if (a->state == AF_IDLE) return -1;

    if (a->state == AF_RUN) {
        // The session this slot owns, chosen at the last possible moment --
        // see the note at the top of this section and at the top of tls.c.
        if (a->tls && !tls_use(slot)) { a->result = -1; a->state = AF_DONE; }
    }

    if (a->state == AF_RUN) {
        af_spawning = slot;
        af_slice_end = now_us() + AF_SLICE_US;
        af_running = 1;
        af_current = slot;
        task_resume(a->task);
        af_current = -1;
        af_running = 0;
        if (a->state == AF_RUN) {
            // Bytes off the wire since this fetch began. With four of them
            // running the figure is shared, which is honest enough for a
            // progress bar and cheaper than counting per socket.
            if (progress) {
                int n = (int)tcp_bytes_in - a->bytes0;
                if (n < a->progress) n = a->progress;
                *progress = n;
            }
            return NET_FETCH_PENDING;
        }
    }
    a->progress = a->result > 0 ? a->result : 0;
    if (progress) *progress = a->progress;
    return a->result;
}

/* Finish an abandoned fetch here and now.
 *
 * "Here" is the point: this runs inside the system call of the program that
 * is giving up, so the coroutine still has a live stack to return to. Leaving
 * it suspended instead is what reset the machine -- see the note at the top of
 * this section.
 *
 * The connection is shut first so the fetch fails out of its read in a slice
 * or two rather than sitting out a ten-second timeout, and then it is run to
 * completion. The answer, if one arrives anyway, is thrown away. */
void net_fetch_cancel(int slot) {
    if (slot < 0 || slot >= AF_SLOTS) return;
    af_slot_t *a = &afs[slot];
    if (a->state == AF_IDLE) return;

    if (a->state == AF_RUN) {
        if (a->sock >= 0) {
            tcp_conn_t *c = conn_of(a->sock);
            if (c) { c->remote_closed = 1; c->state = TCP_CLOSED; }
        }
        /* Bounded by the clock: what matters is how long the program is
         * held up leaving, not how many turns the fetch was given. With its
         * connection shut it comes home in a slice or two; the three seconds
         * are for the case where it does not, and a lost slot is in any case
         * better than a reset machine. */
        uint64_t give_up = now_ms() + 3000;
        while (a->state == AF_RUN && now_ms() < give_up) {
            if (a->tls && !tls_use(slot)) break;
            af_spawning = slot;
            af_slice_end = now_us() + AF_SLICE_US;
            af_running = 1;
            af_current = slot;
            task_resume(a->task);
            af_current = -1;
            af_running = 0;
        }
    }
    if (a->state != AF_RUN) a->state = AF_IDLE;
}

int net_fetch_take(int slot, char *dst, int max) {
    if (slot < 0 || slot >= AF_SLOTS) return -1;
    af_slot_t *a = &afs[slot];
    int n = a->result;
    if (n > max) n = max;
    if (n > 0) nmemcpy(dst, a->buf, n);
    a->state = AF_IDLE;
    return a->result;
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

// A wait in a program's own system call (not a fetch on its own stack, and
// not in the middle of a packet) gives way to other programs every couple of
// milliseconds -- the server it is talking to may be one of them.
static void net_give_way(void) {
    if (af_running || rx_depth || !process_current()) return;
    static uint64_t last;
    uint64_t t = now_us();
    if (t - last < 2000) return;
    last = t;
    // Interrupts are off for the length of a syscall, so the timer that
    // wakes a program sleeping on its own wait -- the server, in its accept()
    // -- would never come. Let a pending one in (the lock is ours already:
    // it is taken again, not waited for), then give way.
    __asm__ volatile ("sti; nop; nop; cli" ::: "memory");
    process_yield();
}

int net_http_get(const char *host, uint32_t ip, uint16_t port,
                 const char *path, char *buf, int max, int keep_headers,
                 const char *post, int postlen, const char *xhdr) {
    if (!up) return -1;
    uint64_t started = now_ms();
    // This path keeps the whole response in the socket until it has all
    // arrived, so it asks for a large buffer; TLS, which drains each record
    // as it lands, asks for a small one.
    int h = tcp_start(ip, port, 262144, 0);
    if (h >= 0) {
        tcp_conn_t *oc = &conns[h];
        while (oc->state == TCP_SYN_SENT) { net_poll(); net_give_way(); }
        if (oc->state == TCP_ESTABLISHED) af_note_socket(h);
        else { tcp_release(oc); h = -1; }
    }
    if (h < 0) {
        net_log_add(host, path, -1, 0, (uint32_t)started, NET_LOG_FAIL);
        return -1;
    }
    tcp_conn_t *c = &conns[h];

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
    tcp_send_data(c, req, n);
    if (postlen > 0) tcp_send_data(c, post, postlen);

    // Drain until the peer closes or we time out with no progress.
    uint64_t last = now_ms();
    int seen = c->rx_len;
    while (!c->remote_closed) {
        net_poll();
        net_give_way();
        if (c->rx_len != seen) { seen = c->rx_len; last = now_ms(); }
        if (now_ms() - last > 4000) break;         // stall timeout
        if (c->rx_len >= c->rx_cap) break;
    }
    tcp_close(c);

    // Strip headers: find the blank line. A caller that asked for them keeps
    // everything from the status line on.
    uint8_t *rx = c->rx + c->rx_head;   // written into when the body is rejoined
    int rxlen = c->rx_len;
    int status = 0;
    if (rxlen > 12 && rx[0] == 'H')
        status = (rx[9] - '0') * 100 + (rx[10] - '0') * 10 + (rx[11] - '0');

    int body = 0;
    for (int i = 0; i + 3 < rxlen; i++) {
        if (rx[i] == '\r' && rx[i+1] == '\n' &&
            rx[i+2] == '\r' && rx[i+3] == '\n') { body = i + 4; break; }
    }
    int hdr = body;

    // The body may have arrived in pieces, each behind its own length in
    // hexadecimal. Undone in place -- the headers sit in front of it and are
    // not disturbed -- and then the header that said so is renamed, because a
    // caller reading it would otherwise be told to undo it a second time.
    // This is the same treatment the TLS path gives it, from the same code.
    int blen = rxlen - hdr;
    if (blen < 0) blen = 0;
    if (http_is_chunked(rx, hdr)) {
        blen = http_dechunk(rx + hdr, blen);
        http_mark_dechunked(rx, hdr);
    }

    int n2 = keep_headers ? hdr + blen : blen;
    const uint8_t *src = keep_headers ? rx : rx + hdr;
    if (n2 > max) n2 = max;
    nmemcpy(buf, src, n2);
    net_tcp_release(h);
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

static void handle_udp(uint32_t srcip, uint32_t dstip, const uint8_t *p, int len) {
    if (len < (int)sizeof(udp_hdr_t)) return;
    const udp_hdr_t *u = (const udp_hdr_t *)p;
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
    // A reply somebody is waiting for, into its own box. Nobody waiting on
    // the port means nobody asked, and it is dropped.
    for (int i = 0; i < UDP_BOXES; i++) {
        udp_box_t *b = &udp_boxes[i];
        if (b->port != dport || b->have) continue;
        if (dlen > (int)sizeof b->data) dlen = sizeof b->data;
        nmemcpy(b->data, data, dlen);
        b->len = dlen;
        b->sip = srcip;
        b->have = 1;
        return;
    }
    // A program's socket on that port.
    udp_deliver(srcip, dstip, ntohs(u->src_port), dport, data, dlen);
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
    // Accept traffic to us, to broadcast (DHCP), and to ourselves.
    if (our_ip && dst != our_ip && dst != 0xFFFFFFFF && !is_loop(dst)) return;
    // Without an address yet, the only conversation from outside is DHCP's.
    if (!up && !is_loop(dst) && ih->proto == IPPROTO_TCP) return;
    int plen = (int)ntohs(ih->total_len) - ihl;
    if (plen < 0 || plen > len - ihl) plen = len - ihl;
    const uint8_t *payload = p + ihl;
    switch (ih->proto) {
        case IPPROTO_ICMP: handle_icmp(src, payload, plen); break;
        case IPPROTO_UDP:  handle_udp(src, dst, payload, plen); break;
        case IPPROTO_TCP:  tcp_input(src, dst, payload, plen); break;
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
    rx_depth++;
    if (type == ETH_ARP) handle_arp(payload, plen);
    else if (type == ETH_IP) handle_ip(payload, plen);
    rx_depth--;
}

/* net_poll is the single place every wait in this file and in tls.c passes
 * through, which makes it the one place that needs to know about slices --
 * af_running and af_slice_end are set by the async section above. */
// The packets waiting on the loopback, into the input side. Not nested: a
// reply one of them makes is queued and taken by the same loop.
static int loop_draining;
static uint8_t loop_pkt[1500];
static void loop_drain(void) {
    if (!loop_n || loop_draining) return;
    loop_draining = 1;
    for (int budget = 256; loop_n && budget; budget--) {
        loop_pkt_t *q = &loopq[loop_head];
        int len = q->len;
        nmemcpy(loop_pkt, q->pkt, len);
        loop_head = (loop_head + 1) % LOOP_Q;
        loop_n--;
        rx_depth++;
        handle_ip(loop_pkt, len);
        rx_depth--;
    }
    loop_draining = 0;
}

void net_poll(void) {
    // Never the card inside the card: a nested poll would start on the
    // receive ring the outer one is still walking.
    static int polling;
    if (!polling) {
        polling = 1;
        nic_poll();
        polling = 0;
    }
    loop_drain();
    tcp_timers();
    if (af_running && !rx_depth && now_us() >= af_slice_end) task_yield_back();
}

// From the scheduler's loop on the first core, between processes: keeps the
// connections programs hold moving -- answers, acknowledgements, resends --
// while those programs are busy with something else, or asleep.
void net_service(void) {
    static uint64_t last;
    if (!loop_n && !net_active()) return;
    uint64_t now = now_ms();
    if (now == last) return;
    last = now;
    net_poll();
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

// The card the network was up on is gone (a phone pulled out): the network
// is down, and the next net_up chooses a card again.
static void check_nic(void) {
    if (have_nic && nic_gone()) {
        kprintf("net: %s went away\n", nic_name() ? nic_name() : "the card");
        up = 0;
        have_nic = 0;
        nic_unbind();
    }
}

int net_up(void) {
    check_nic();
    if (up) return 1;
    // A card chosen earlier that still has no link (the wired one, no cable)
    // is not kept: something plugged in since -- a phone -- may have one.
    if (have_nic && !nic_link()) { have_nic = 0; nic_unbind(); }
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
        // The network is the one thing that knows what time it is.
        clock_sync(0, 1);
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

int net_is_up(void) { check_nic(); return up; }

void net_config(uint32_t *ip, uint32_t *gw, uint32_t *mask, uint32_t *dns) {
    if (ip) *ip = our_ip;
    if (gw) *gw = gw_ip;
    if (mask) *mask = net_mask;
    if (dns) *dns = dns_ip;
}

const uint8_t *net_mac(void) { return have_nic ? our_mac : NULL; }
