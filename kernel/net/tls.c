#include "tls.h"
#include "net.h"
#include "http.h"
#include "../crypto/crypto.h"
#include "x509.h"
#include "../kernel/kio.h"
#include "../mm/heap.h"
#include <stddef.h>

/* TLS 1.3 (RFC 8446), client side.
 *
 * The handshake is one round trip: we send a ClientHello carrying a key share,
 * the server answers with its own share and -- already encrypted with the keys
 * both sides can now derive -- its certificate and a Finished message. We
 * reply with our Finished and everything after that is application data under
 * a second set of keys.
 *
 * The transcript hash is the thread running through all of it: almost every
 * secret is derived from "the hash of every handshake message so far", so the
 * running SHA-256 in `tr` has to be fed exactly the handshake bytes, in order,
 * with no record headers and no gaps. Most of the ways this can go wrong end
 * with the server's Finished not matching ours and no clue as to why. */

/* --- small helpers -------------------------------------------------------- */

static void tmemcpy(void *d, const void *s, int n) {
    uint8_t *a = d; const uint8_t *b = s;
    for (int i = 0; i < n; i++) a[i] = b[i];
}
static void tmemset(void *d, int v, int n) {
    uint8_t *a = d;
    for (int i = 0; i < n; i++) a[i] = (uint8_t)v;
}
static int tstrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* --- state ---------------------------------------------------------------- */

#define REC_MAX   17408          /* 2^14 plaintext + content type + tag + slack */
#define HS_MAX    32768          /* a certificate chain, reassembled */
#define APP_MAX   16384
#define TLS_RESP_MAX (1024 * 1024)   /* one assembled response */

typedef struct {
    int  open;
    int  sock;               /* the connection this session runs on */
    const char *err;

    sha256_t tr;                 /* transcript hash, handshake messages only */

    uint8_t  priv[32], pub[32];
    uint8_t  hs_secret[32], master[32];
    uint8_t  c_hs[32], s_hs[32]; /* handshake traffic secrets */
    uint8_t  c_ap[32], s_ap[32]; /* application traffic secrets */

    aes128gcm_t c_key, s_key;
    uint8_t  c_iv[12], s_iv[12];
    uint64_t c_seq, s_seq;
    int      encrypting;         /* 1 once our records are protected     */
    int      s_keyed;            /* 1 once the server's records are too  */

    uint8_t  hs_buf[HS_MAX];     /* handshake reassembly across records  */
    int      hs_have;

    uint8_t  rec[REC_MAX];
    int      rec_len;
    uint8_t  rec_type;

    uint8_t  app[APP_MAX];       /* decrypted application data not yet read */
    int      app_len, app_off;
    int      peer_closed;

    /* The server's certificate chain, copied out of the handshake buffer
     * because the messages that follow overwrite it. Everything parsed from a
     * certificate points into these bytes. */
    uint8_t  cert_buf[HS_MAX];
    int      cert_len;
    x509_t   leaf;
    int      have_leaf;
    int      verified;           /* the identity was checked and it held up */
    char     host[160];

    /* The response being assembled. A megabyte, and one per session, which
     * is why the whole thing comes off the heap rather than out of BSS. */
    uint8_t  *resp;
} tls_state_t;

/* --- which session is in play ----------------------------------------------
 *
 * See the note at the top of the file: the pointer is moved by tls_use(),
 * called from net.c immediately before a fetch task is resumed, and the
 * cooperative scheduling is what makes that safe. Sessions are allocated on
 * first use because most of the time only one of them is ever needed, and
 * each is the better part of a hundred kilobytes before the response buffer.
 */
#define TLS_SESSIONS 4

typedef struct {
    int      live;
    uint32_t ip;
    uint16_t port;
    char     host[160];
} tls_pool_t;

static tls_state_t *sessions[TLS_SESSIONS];
static tls_pool_t   pools[TLS_SESSIONS];
static tls_state_t *Tp;
static tls_pool_t  *Pp;

#define T    (*Tp)
#define pool (*Pp)

int tls_use(int slot) {
    if (slot < 0 || slot >= TLS_SESSIONS) return 0;
    if (!sessions[slot]) {
        tls_state_t *n = kmalloc(sizeof *n);
        if (!n) return 0;
        tmemset(n, 0, sizeof *n);
        n->sock = -1;
        n->resp = kmalloc(TLS_RESP_MAX);
        if (!n->resp) { kfree(n); return 0; }   /* nothing half-built is kept */
        sessions[slot] = n;
    }
    Tp = sessions[slot];
    Pp = &pools[slot];
    return 1;
}

int tls_session_count(void) { return TLS_SESSIONS; }

/* A default, so the first caller of any entry point cannot find no session
 * at all. Slot zero is what the single-fetch path has always used.
 *
 * It CAN fail -- a session is the better part of a megabyte -- and the answer
 * has to be carried out to every entry point. Ignoring it once cost a morning:
 * Tp stayed NULL, T.resp with it, and the next response was written from
 * address zero upwards across the bottom of the heap. */
static int tls_need_session(void) { return Tp != NULL || tls_use(0); }

const char *tls_error(void) {
    if (!tls_need_session()) return "out of memory for a TLS session";
    return T.err ? T.err : "no error";
}
int tls_verified(void) { return tls_need_session() && T.verified; }

static int fail(const char *why) { T.err = why; return 0; }

/* --- record layer --------------------------------------------------------- */

/* Read exactly one record into T.rec. Returns 1 on success, 0 on failure or
 * a clean close. Encrypted records are decrypted in place and their real
 * content type recovered from the last non-zero byte. */
static int record_read(void) {
    int avail = net_tcp_fill(T.sock, 5, 10000);
    if (avail < 5) {
        /* A server that has said everything it intends to may simply close.
         * That is the end of the conversation, not a failure of it. */
        if (avail == 0 && net_tcp_closed(T.sock)) { T.peer_closed = 1; return 0; }
        return fail("no reply from the server");
    }

    const uint8_t *h = net_tcp_peek(T.sock);
    uint8_t type = h[0];
    int len = ((int)h[3] << 8) | h[4];
    if (len < 0 || len > REC_MAX) return fail("record too large");

    uint8_t hdr[5];
    tmemcpy(hdr, h, 5);

    if (net_tcp_fill(T.sock, 5 + len, 10000) < 5 + len) return fail("truncated record");
    tmemcpy(T.rec, net_tcp_peek(T.sock) + 5, len);
    net_tcp_consume(T.sock, 5 + len);
    T.rec_len = len;
    T.rec_type = type;

    /* A ChangeCipherSpec after the handshake has begun exists only so that old
     * middleboxes see something familiar. It carries no meaning. */
    if (type == 20) { T.rec_len = 0; return record_read(); }

    if (type == 23 && T.s_keyed) {
        /* Encrypted. The nonce is the static IV with the sequence number xored
         * into its last eight bytes; the header is the associated data. */
        if (len < 17) return fail("short encrypted record");
        uint8_t nonce[12];
        tmemcpy(nonce, T.s_iv, 12);
        for (int i = 0; i < 8; i++)
            nonce[11 - i] ^= (uint8_t)(T.s_seq >> (i * 8));
        T.s_seq++;

        int plen = len - 16;
        if (!aes128gcm_open(&T.s_key, nonce, hdr, 5, T.rec, plen, T.rec + plen))
            return fail("the server's record did not authenticate");

        /* The real content type is the last byte that is not padding. */
        while (plen > 0 && T.rec[plen - 1] == 0) plen--;
        if (plen == 0) return fail("empty inner record");
        T.rec_type = T.rec[plen - 1];
        T.rec_len = plen - 1;
    }

    if (T.rec_type == 21) {                 /* alert */
        if (T.rec_len >= 2 && T.rec[1] == 0) { T.peer_closed = 1; return 0; }
        return fail("the server sent an alert");
    }
    return 1;
}

/* One record's worth of scratch. Static because the kernel stack is measured
 * in a few thousand bytes and a TLS record is measured in sixteen thousand. */
static uint8_t wbuf[REC_MAX + 5];
static uint8_t hs_msg[HS_MAX];

static int record_write(uint8_t type, const void *data, int len) {
    uint8_t *out = wbuf;

    if (!T.encrypting) {
        out[0] = type;
        out[1] = 0x03; out[2] = 0x01;       /* legacy version on the wire */
        out[3] = (uint8_t)(len >> 8); out[4] = (uint8_t)len;
        tmemcpy(out + 5, data, len);
        return net_tcp_write(T.sock, out, len + 5) == len + 5;
    }

    /* Protected: the plaintext gains its real type as a trailing byte, and the
     * header advertises the length after encryption. */
    int inner = len + 1;
    int total = inner + 16;
    out[0] = 23;                            /* always "application data" */
    out[1] = 0x03; out[2] = 0x03;
    out[3] = (uint8_t)(total >> 8); out[4] = (uint8_t)total;
    tmemcpy(out + 5, data, len);
    out[5 + len] = type;

    uint8_t nonce[12];
    tmemcpy(nonce, T.c_iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)(T.c_seq >> (i * 8));
    T.c_seq++;

    aes128gcm_seal(&T.c_key, nonce, out, 5, out + 5, inner, out + 5 + inner);
    return net_tcp_write(T.sock, out, 5 + total) == 5 + total;
}

/* --- the key schedule (RFC 8446 7.1) -------------------------------------- */

static void set_keys(const uint8_t secret[32], aes128gcm_t *c, uint8_t iv[12]) {
    uint8_t key[16];
    hkdf_expand_label(secret, "key", 0, 0, key, 16);
    hkdf_expand_label(secret, "iv", 0, 0, iv, 12);
    aes128gcm_init(c, key);
}

static void derive_handshake_keys(const uint8_t shared[32]) {
    uint8_t zeros[32], early[32], derived[32], empty[32], th[32];
    tmemset(zeros, 0, 32);

    sha256(0, 0, empty);                                   /* hash of nothing */
    hkdf_extract(0, 0, zeros, 32, early);
    tls13_derive_secret(early, "derived", empty, derived);
    hkdf_extract(derived, 32, shared, 32, T.hs_secret);

    sha256_peek(&T.tr, th);                                /* ...ServerHello */
    tls13_derive_secret(T.hs_secret, "c hs traffic", th, T.c_hs);
    tls13_derive_secret(T.hs_secret, "s hs traffic", th, T.s_hs);

    set_keys(T.c_hs, &T.c_key, T.c_iv);
    set_keys(T.s_hs, &T.s_key, T.s_iv);
    T.c_seq = T.s_seq = 0;
    T.s_keyed = 1;
}

static void derive_app_keys(void) {
    uint8_t derived[32], zeros[32], empty[32], th[32];
    tmemset(zeros, 0, 32);
    sha256(0, 0, empty);

    tls13_derive_secret(T.hs_secret, "derived", empty, derived);
    hkdf_extract(derived, 32, zeros, 32, T.master);

    sha256_peek(&T.tr, th);                        /* ...server Finished */
    tls13_derive_secret(T.master, "c ap traffic", th, T.c_ap);
    tls13_derive_secret(T.master, "s ap traffic", th, T.s_ap);
}

/* --- the ClientHello ------------------------------------------------------ */

static int put_ext(uint8_t *p, int *n, uint16_t type, const uint8_t *body, int len) {
    p[(*n)++] = (uint8_t)(type >> 8); p[(*n)++] = (uint8_t)type;
    p[(*n)++] = (uint8_t)(len >> 8);  p[(*n)++] = (uint8_t)len;
    tmemcpy(p + *n, body, len);
    *n += len;
    return 4 + len;
}

static int send_client_hello(const char *host) {
    uint8_t msg[1024];
    int n = 0;

    msg[n++] = 1;                              /* ClientHello */
    int lenpos = n; n += 3;                    /* 24-bit length, filled below */

    msg[n++] = 0x03; msg[n++] = 0x03;          /* legacy_version = TLS 1.2 */

    crypto_random(msg + n, 32); n += 32;       /* random */

    /* A non-empty legacy session id makes the exchange look like a resumed
     * TLS 1.2 session to anything watching, which is what gets it through
     * middleboxes that have not heard of 1.3. */
    msg[n++] = 32;
    crypto_random(msg + n, 32); n += 32;

    msg[n++] = 0; msg[n++] = 2;                /* cipher suites, 2 bytes */
    msg[n++] = 0x13; msg[n++] = 0x01;          /* TLS_AES_128_GCM_SHA256 */

    msg[n++] = 1; msg[n++] = 0;                /* no compression */

    int extpos = n; n += 2;                    /* extensions length */
    uint8_t body[512];
    int b;

    /* server_name: the certificate a server picks depends on this. */
    {
        int hl = tstrlen(host);
        b = 0;
        body[b++] = (uint8_t)((hl + 3) >> 8); body[b++] = (uint8_t)(hl + 3);
        body[b++] = 0;                                     /* host_name */
        body[b++] = (uint8_t)(hl >> 8); body[b++] = (uint8_t)hl;
        tmemcpy(body + b, host, hl); b += hl;
        put_ext(msg, &n, 0, body, b);
    }
    /* supported_groups: x25519 only */
    { uint8_t e[] = { 0, 2, 0x00, 0x1d }; put_ext(msg, &n, 10, e, 4); }
    /* signature_algorithms: we do not check signatures yet, but a server will
     * refuse to continue without being told what we would accept. */
    { uint8_t e[] = { 0, 8, 0x08, 0x04, 0x04, 0x03, 0x05, 0x03, 0x04, 0x01 };
      put_ext(msg, &n, 13, e, 10); }
    /* supported_versions: TLS 1.3 */
    { uint8_t e[] = { 2, 0x03, 0x04 }; put_ext(msg, &n, 43, e, 3); }
    /* key_share: our X25519 public value */
    {
        b = 0;
        body[b++] = 0; body[b++] = 36;                     /* client_shares */
        body[b++] = 0x00; body[b++] = 0x1d;                /* x25519 */
        body[b++] = 0; body[b++] = 32;
        tmemcpy(body + b, T.pub, 32); b += 32;
        put_ext(msg, &n, 51, body, b);
    }

    int extlen = n - extpos - 2;
    msg[extpos] = (uint8_t)(extlen >> 8); msg[extpos + 1] = (uint8_t)extlen;

    int bodylen = n - lenpos - 3;
    msg[lenpos]     = (uint8_t)(bodylen >> 16);
    msg[lenpos + 1] = (uint8_t)(bodylen >> 8);
    msg[lenpos + 2] = (uint8_t)bodylen;

    sha256_update(&T.tr, msg, (uint32_t)n);
    return record_write(22, msg, n);
}

/* --- reading the server's side -------------------------------------------- */

/* One handshake message, reassembled across records. `out` receives the whole
 * message including its four-byte header. */
static int hs_read(uint8_t *out, int max, int *len) {
    for (;;) {
        if (T.hs_have >= 4) {
            int mlen = ((int)T.hs_buf[1] << 16) | ((int)T.hs_buf[2] << 8) | T.hs_buf[3];
            if (T.hs_have >= 4 + mlen) {
                if (4 + mlen > max) return fail("handshake message too large");
                tmemcpy(out, T.hs_buf, 4 + mlen);
                *len = 4 + mlen;
                T.hs_have -= 4 + mlen;
                for (int i = 0; i < T.hs_have; i++)
                    T.hs_buf[i] = T.hs_buf[i + 4 + mlen];
                return 1;
            }
        }
        if (!record_read()) return 0;
        if (T.rec_type != 22) return fail("expected a handshake message");
        if (T.hs_have + T.rec_len > HS_MAX) return fail("handshake buffer overflow");
        tmemcpy(T.hs_buf + T.hs_have, T.rec, T.rec_len);
        T.hs_have += T.rec_len;
    }
}

static int read_server_hello(uint8_t shared[32]) {
    uint8_t *msg = hs_msg;
    int len;

    if (!hs_read(msg, HS_MAX, &len)) return 0;
    if (msg[0] != 2) return fail("the server did not send a ServerHello");

    /* A HelloRetryRequest is a ServerHello with this exact random value. It
     * means the server wants a different group; we only have one to offer. */
    static const uint8_t hrr[32] = {
        0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
        0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };
    if (len >= 38 && crypto_equal(msg + 6, hrr, 32))
        return fail("the server asked for a group we do not have");

    int p = 4 + 2 + 32;                      /* header, version, random */
    if (p >= len) return fail("short ServerHello");
    int sid = msg[p++];
    p += sid;
    if (p + 3 > len) return fail("short ServerHello");
    uint16_t suite = (uint16_t)((msg[p] << 8) | msg[p + 1]); p += 2;
    p += 1;                                  /* compression */
    if (suite != 0x1301) return fail("the server chose a cipher we do not have");

    if (p + 2 > len) return fail("no extensions in ServerHello");
    int extlen = (msg[p] << 8) | msg[p + 1]; p += 2;
    int end = p + extlen;
    if (end > len) return fail("bad extension length");

    int got_share = 0, got_version = 0;
    while (p + 4 <= end) {
        uint16_t et = (uint16_t)((msg[p] << 8) | msg[p + 1]);
        int el = (msg[p + 2] << 8) | msg[p + 3];
        p += 4;
        if (p + el > end) return fail("bad extension");
        if (et == 43) {                                    /* supported_versions */
            if (el == 2 && msg[p] == 0x03 && msg[p + 1] == 0x04) got_version = 1;
        } else if (et == 51) {                             /* key_share */
            if (el == 36 && msg[p] == 0x00 && msg[p + 1] == 0x1d) {
                x25519(shared, T.priv, msg + p + 4);
                got_share = 1;
            }
        }
        p += el;
    }
    if (!got_version) return fail("the server does not speak TLS 1.3");
    if (!got_share) return fail("the server sent no usable key share");

    sha256_update(&T.tr, msg, (uint32_t)len);
    return 1;
}

/* The Certificate message: a context byte string nobody uses, then a list of
 * certificates, each with a length and a block of extensions to skip. */
static int check_certificates(void) {
    const uint8_t *b = T.cert_buf;
    int len = T.cert_len;
    if (len < 8) return fail("the certificate message is malformed");

    int p = 4;                                   /* past the handshake header */
    int ctxlen = b[p];
    p += 1 + ctxlen;
    if (p + 3 > len) return fail("the certificate message is malformed");
    int listlen = ((int)b[p] << 16) | ((int)b[p + 1] << 8) | b[p + 2];
    p += 3;
    int end = p + listlen;
    if (end > len) return fail("the certificate message is truncated");

    const uint8_t *certs[8];
    int lens[8], n = 0;

    while (p < end && n < 8) {
        if (p + 3 > end) break;
        int clen = ((int)b[p] << 16) | ((int)b[p + 1] << 8) | b[p + 2];
        p += 3;
        if (clen <= 0 || p + clen > end) return fail("a certificate is truncated");
        certs[n] = b + p;
        lens[n] = clen;
        n++;
        p += clen;
        if (p + 2 > end) break;
        int extlen = ((int)b[p] << 8) | b[p + 1];
        p += 2 + extlen;
    }
    if (n == 0) return fail("the server sent no certificate");

    /* The first one is the server's own; the rest are there to connect it to
     * something we trust. */
    if (!x509_parse(certs[0], lens[0], &T.leaf))
        return fail("the server's certificate could not be read");
    T.have_leaf = 1;

    const char *why = 0;
    if (!x509_verify_chain(certs, lens, n, T.host, &why)) {
        T.err = why && why[0] ? why : "the certificate could not be verified";
        return 0;
    }
    return 1;
}

/* CertificateVerify: a signature, made with the key in the certificate we just
 * checked, over the handshake so far. This is what ties the certificate to
 * whoever is actually on the other end of the socket -- without it, anyone
 * could replay a real server's certificate and claim to be it. */
static int check_cert_verify(const uint8_t *msg, int len, const uint8_t th[32]) {
    static uint8_t blob[64 + 34 + 1 + 32];

    if (!T.have_leaf) return fail("CertificateVerify arrived before the certificate");
    if (len < 8) return fail("CertificateVerify is malformed");

    uint16_t scheme = (uint16_t)((msg[4] << 8) | msg[5]);
    int siglen = (msg[6] << 8) | msg[7];
    if (8 + siglen > len) return fail("CertificateVerify is truncated");

    /* RFC 8446 4.4.3: sixty-four spaces, a context string, a zero, the hash. */
    int n = 0;
    for (int i = 0; i < 64; i++) blob[n++] = 0x20;
    const char *ctx = "TLS 1.3, server CertificateVerify";
    for (int i = 0; ctx[i]; i++) blob[n++] = (uint8_t)ctx[i];
    blob[n++] = 0;
    for (int i = 0; i < 32; i++) blob[n++] = th[i];

    if (!x509_verify_sig(&T.leaf, scheme, blob, n, msg + 8, siglen))
        return fail("the server could not prove it owns its certificate");
    return 1;
}

/* Everything from EncryptedExtensions up to and including the server's
 * Finished, which is the message that proves it holds the private key. */
static int read_server_flight(void) {
    uint8_t *msg = hs_msg;
    int len;
    uint8_t th_before_finished[32];

    for (;;) {
        /* The transcript for the Finished check must NOT include the Finished
         * message itself, so take the snapshot before feeding it in. */
        sha256_peek(&T.tr, th_before_finished);

        if (!hs_read(msg, HS_MAX, &len)) return 0;
        uint8_t type = msg[0];

        if (type == 20) {                                  /* Finished */
            uint8_t fkey[32], want[32];
            hkdf_expand_label(T.s_hs, "finished", 0, 0, fkey, 32);
            hmac_sha256(fkey, 32, th_before_finished, 32, want);
            if (len != 36 || !crypto_equal(msg + 4, want, 32))
                return fail("the server's Finished did not match");
            sha256_update(&T.tr, msg, (uint32_t)len);
            return 1;
        }

        if (type == 11) {                                  /* Certificate */
            if (len > HS_MAX) return fail("the certificate chain is too large");
            for (int i = 0; i < len; i++) T.cert_buf[i] = msg[i];
            T.cert_len = len;
            sha256_update(&T.tr, msg, (uint32_t)len);
            if (!check_certificates()) return 0;
            continue;
        }
        if (type == 15) {                                  /* CertificateVerify */
            /* The signature covers the transcript UP TO but not including
             * itself, which is exactly the snapshot taken above. */
            if (!check_cert_verify(msg, len, th_before_finished)) return 0;
            sha256_update(&T.tr, msg, (uint32_t)len);
            T.verified = 1;
            continue;
        }

        /* EncryptedExtensions(8) and CertificateRequest(13) go into the
         * transcript unread. */
        sha256_update(&T.tr, msg, (uint32_t)len);
        if (type != 8 && type != 13)
            return fail("unexpected handshake message");
    }
}

static int send_finished(void) {
    uint8_t fkey[32], th[32], msg[36];
    hkdf_expand_label(T.c_hs, "finished", 0, 0, fkey, 32);
    sha256_peek(&T.tr, th);

    msg[0] = 20; msg[1] = 0; msg[2] = 0; msg[3] = 32;
    hmac_sha256(fkey, 32, th, 32, msg + 4);
    /* Our own Finished does go into the transcript, but nothing after the
     * handshake reads it, so this is for completeness. */
    sha256_update(&T.tr, msg, 36);
    return record_write(22, msg, 36);
}

/* --- the public face ------------------------------------------------------ */

int tls_connect(uint32_t ip, uint16_t port, const char *host) {
    if (!tls_need_session()) return 0;
    uint8_t shared[32];

    /* The session is cleared for the new connection, but the response
     * buffer belongs to the SESSION and outlives every connection made on
     * it. Forgetting that here nulled the pointer and sent the next response
     * to address zero. */
    uint8_t *keep_resp = T.resp;
    tmemset(&T, 0, sizeof T);
    T.resp = keep_resp;
    T.sock = -1;
    T.err = 0;
    sha256_init(&T.tr);

    {   /* Kept for the certificate check, which happens mid-handshake. */
        int i = 0;
        while (host[i] && i < (int)sizeof T.host - 1) { T.host[i] = host[i]; i++; }
        T.host[i] = 0;
    }

    crypto_random(T.priv, 32);
    x25519_base(T.pub, T.priv);

    /* This buffer IS the receive window -- that is what makes the number
     * matter. It was 32 KiB for a while, on the reasoning that every record
     * is drained into T.app as it lands so the socket need not hold much.
     * True, and beside the point: with a 32 KiB window the sender stops
     * every 32 KiB and waits to be told there is room, and the answer only
     * goes out when this fetch next gets a turn. Measured on github.com,
     * that took a 209 KB image FIFTY-FIVE SECONDS. At 256 KiB the same
     * image arrives in 160 ms -- better than a megabyte a second. */
    T.sock = net_tcp_open(ip, port, 262144);
    if (T.sock < 0) return fail("could not connect");

    if (!send_client_hello(host)) return fail("could not send the hello");
    if (!read_server_hello(shared)) return 0;

    derive_handshake_keys(shared);
    if (!read_server_flight()) return 0;

    /* A server that never sent a CertificateVerify never proved anything. The
     * only legitimate way to skip it is a resumed session, which we never ask
     * for. */
    if (!T.verified) return fail("the server did not prove its identity");

    /* The application secrets are taken from the transcript as it stands NOW,
     * ending at the server's Finished. Our own Finished comes after them in
     * time but not in the transcript they are derived from -- deriving after
     * sending it produces keys that look perfectly valid here and that the
     * server cannot read, which it answers with an alert and no explanation. */
    derive_app_keys();

    /* Our Finished is the last message under the handshake keys, so the switch
     * to the application keys happens after it has gone out. */
    T.encrypting = 1;
    if (!send_finished()) return fail("could not send Finished");

    set_keys(T.c_ap, &T.c_key, T.c_iv);
    set_keys(T.s_ap, &T.s_key, T.s_iv);
    T.c_seq = T.s_seq = 0;

    T.open = 1;
    return 1;
}

int tls_write(const void *data, int len) {
    if (!Tp) return -1;
    if (!T.open) return -1;
    const uint8_t *p = data;
    int left = len;
    while (left > 0) {
        int chunk = left > 16384 ? 16384 : left;
        if (!record_write(23, p, chunk)) return -1;
        p += chunk; left -= chunk;
    }
    return len;
}

int tls_read(void *buf, int max) {
    if (!Tp) return -1;
    if (!T.open) return -1;

    while (T.app_off >= T.app_len) {
        if (T.peer_closed) return 0;
        T.app_off = T.app_len = 0;
        if (!record_read()) return T.peer_closed ? 0 : -1;

        if (T.rec_type == 23) {
            int n = T.rec_len;
            if (n > APP_MAX) n = APP_MAX;
            tmemcpy(T.app, T.rec, n);
            T.app_len = n;
        } else if (T.rec_type == 22) {
            /* NewSessionTicket and friends arrive after the handshake and are
             * of no use to a client that never resumes. */
            continue;
        } else {
            return -1;
        }
        if (net_tcp_closed(T.sock) && net_tcp_avail(T.sock) == 0 && T.app_len == 0)
            return 0;
    }

    int n = T.app_len - T.app_off;
    if (n > max) n = max;
    tmemcpy(buf, T.app + T.app_off, n);
    T.app_off += n;
    return n;
}

void tls_close(void) {
    if (!Tp) return;
    if (T.open) {
        uint8_t alert[2] = { 1, 0 };          /* warning, close_notify */
        record_write(21, alert, 2);
    }
    /* The handle goes back here and nowhere else: this is the single
     * teardown path, which is why the pool can be as simple as it is. */
    net_tcp_release(T.sock);
    T.sock = -1;
    T.open = 0;
}

/* --- keeping the connection ------------------------------------------------
 *
 * A page is one document and then twenty pictures, nearly all of them from the
 * same two or three hosts. Closing the connection after each one meant a fresh
 * TCP handshake, a fresh X25519 exchange and a fresh certificate chain to
 * verify -- per picture. Twenty-one handshakes for one page of GitHub, and the
 * cryptography is the expensive part.
 *
 * So the connection is kept. Only one can be held, because the TLS state above
 * is a single session, and it is remembered by where it goes: another request
 * to the same address, port and name walks straight past the handshake.
 *
 * The price is that we must now know exactly where the body ends, since the
 * connection closing is no longer the answer. That is what Content-Length and
 * the chunk framing are read for. */
/* Tracing goes to the serial port and nowhere else -- kprintf would draw it
 * over the window the user is looking at, which is the whole reason the
 * request log exists. Off unless something is being chased. */
#define TLS_TRACE 0
#if TLS_TRACE
void serial_write(const char *s);
static void tnum(char *o, int *n, long v) {
    if (v < 0) { o[(*n)++] = '-'; v = -v; }
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) o[(*n)++] = t[--k];
}
static void trace(const char *tag, const char *host, long a, long b, long c) {
    char o[300]; int n = 0;
    for (const char *p = tag; *p && n < 40; p++) o[n++] = *p;
    o[n++] = ' ';
    for (const char *p = host; *p && n < 200; p++) o[n++] = *p;
    o[n++] = ' '; tnum(o, &n, a);
    o[n++] = ' '; tnum(o, &n, b);
    o[n++] = ' '; tnum(o, &n, c);
    o[n++] = '\n'; o[n] = 0;
    serial_write(o);
}
#else
#define trace(a,b,c,d,e) ((void)0)
#endif

static int host_same(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* 1 if an open connection to this exact place is available. Anything else
 * open is closed here, since only one can be held. */
static int pool_take(uint32_t ip, uint16_t port, const char *host) {
    if (pool.live && T.open && !T.peer_closed && !net_tcp_closed(T.sock) &&
        pool.ip == ip && pool.port == port && host_same(pool.host, host))
        return 1;
    if (pool.live || T.open) { tls_close(); pool.live = 0; }
    return 0;
}

static void pool_hold(uint32_t ip, uint16_t port, const char *host) {
    pool.live = 1; pool.ip = ip; pool.port = port;
    int i = 0;
    while (host[i] && i < (int)sizeof pool.host - 1) { pool.host[i] = host[i]; i++; }
    pool.host[i] = 0;
}

static void pool_drop(void) {
    if (T.open || pool.live) tls_close();
    pool.live = 0;
}

void tls_pool_flush(void) { if (tls_need_session()) pool_drop(); }

int tls_https_get(const char *host, uint32_t ip, uint16_t port,
                  const char *path, char *buf, int max, int keep_headers,
                  const char *body, int blen, const char *xhdr) {
    if (!tls_need_session()) return -1;
    /* Big enough for a photograph. The page itself is small; what is
     * fetched next to it is not. */
    uint8_t *resp = T.resp;
    const int cap = TLS_RESP_MAX;
    if (!resp) return -1;      /* no buffer, no answer -- never write blind */

    /* Twice at most: a connection we kept may have been closed by the server
     * while it sat idle, and that only shows up when we try to use it. The
     * second attempt always starts from a fresh handshake. */
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t started = net_ms();
        int reused = pool_take(ip, port, host);
        unsigned lf = NET_LOG_TLS | (reused ? NET_LOG_REUSED : 0);

        if (!reused) {
            if (!tls_connect(ip, port, host)) {
                net_log_add(host, path, -1, 0, started, lf | NET_LOG_FAIL);
                pool_drop();
                return -1;
            }
            pool_hold(ip, port, host);
        }

        /* --- the request ---------------------------------------------------
         * Compression is asked for: the browser undoes it, and asking a
         * server not to compress is asking it to send three or four times
         * the bytes. Anything we cannot undo still arrives as itself,
         * because identity is offered too. */
        char req[2816];      /* the cookies a site sets can be long */
        int n = 0;
        /* Not `clen`: further down that is the length the SERVER declares,
         * and the two would be the same name for opposite directions. */
        /* The two header names alone are sixty-five characters, so a
     * sixty-four byte buffer cut this in half and the length never
     * reached the server -- which read the body as empty. */
    char lenhdr[128];
        lenhdr[0] = 0;
        if (blen > 0) {
            const char *pre = "Content-Type: application/x-www-form-urlencoded\r\n"
                              "Content-Length: ";
            int c = 0;
            while (*pre && c < (int)sizeof lenhdr - 16) lenhdr[c++] = *pre++;
            char d[12];
            int dn = 0, v = blen;
            do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (dn) lenhdr[c++] = d[--dn];
            lenhdr[c++] = '\r'; lenhdr[c++] = '\n';
            lenhdr[c] = 0;
        }
        const char *parts[] = { blen > 0 ? "POST " : "GET ", path,
                                " HTTP/1.1\r\nHost: ", host,
                                "\r\nConnection: keep-alive\r\n"
                                "Accept-Encoding: gzip, deflate, identity\r\n"
                                /* What we can actually decode. Without this a
                                 * negotiating server sends WebP, which we
                                 * cannot read at all. */
                                "Accept: text/html,application/xhtml+xml,text/css,image/png,image/jpeg,image/gif,image/bmp,*/*;q=0.5\r\n"
                                "User-Agent: xyuos\r\n", lenhdr,
                                xhdr ? xhdr : "", "\r\n" };
        for (int i = 0; i < 8; i++) {
            const char *s = parts[i];
            while (*s && n < (int)sizeof(req) - 1) req[n++] = *s++;
        }
        if (tls_write(req, n) != n ||
            (blen > 0 && tls_write(body, blen) != blen)) {
            pool_drop();
            if (reused) continue;              /* the kept connection was dead */
            net_log_add(host, path, -1, 0, started, lf | NET_LOG_FAIL);
            return -1;
        }

        /* --- the headers -------------------------------------------------- */
        int total = 0, hdr = -1, status = 0;
        for (;;) {
            int scanned = 0;
            hdr = -1;
            while (hdr < 0) {
                for (int i = scanned; i + 3 < total; i++)
                    if (resp[i] == '\r' && resp[i+1] == '\n' &&
                        resp[i+2] == '\r' && resp[i+3] == '\n') { hdr = i + 4; break; }
                if (hdr >= 0 || total >= cap) break;
                scanned = total > 3 ? total - 3 : 0;
                int r = tls_read(resp + total, cap - total);
                if (r <= 0) break;
                total += r;
            }
            if (hdr < 0) break;
            status = http_status(resp, hdr);
            if (status < 100 || status >= 200) break;
            /* A 1xx is a placeholder, not the answer -- the real response
             * follows it down the same connection. */
            for (int i = hdr; i < total; i++) resp[i - hdr] = resp[i];
            total -= hdr;
        }
        if (hdr < 0) {
            pool_drop();
            if (reused && total == 0) continue;
            net_log_add(host, path, -1, status, started, lf | NET_LOG_FAIL);
            return -1;
        }

        /* --- the body, read to its true end ------------------------------- */
        int chunked = http_is_chunked(resp, hdr);
        int clen    = http_content_length(resp, hdr);
        int alive   = http_keeps_alive(resp, hdr);
        int whole   = 1;

        if (status == 204 || status == 304) {
            /* Defined to carry nothing, whatever the headers claim. */
        } else if (chunked) {
            while (!http_chunked_complete(resp + hdr, total - hdr)) {
                if (total >= cap) { whole = 0; break; }
                int r = tls_read(resp + total, cap - total);
                if (r <= 0) { whole = 0; break; }
                total += r;
            }
        } else if (clen >= 0) {
            int want = hdr + clen;
            if (want > cap) { want = cap; whole = 0; }   /* larger than we hold */
            while (total < want) {
                int r = tls_read(resp + total, cap - total);
                if (r <= 0) { whole = 0; break; }
                total += r;
            }
        } else {
            /* No length given at all, so the body ends when the connection
             * does -- and after that there is nothing to keep. */
            while (total < cap) {
                int r = tls_read(resp + total, cap - total);
                if (r <= 0) break;
                total += r;
            }
            alive = 0;
        }

        if (!alive || !whole) pool_drop();

        int blen = total - hdr;
        if (blen < 0) blen = 0;

        /* The chunk lengths are hex digits sitting in the middle of the body;
         * undone in place, so the headers in front of them are undisturbed. */
        if (chunked) {
            blen = http_dechunk(resp + hdr, blen);
            http_mark_dechunked(resp, hdr);
        }

        net_log_add(host, path, blen, status, started,
                    lf | (whole ? 0 : NET_LOG_FAIL));

        if (keep_headers) {
            int n2 = hdr + blen;
            if (n2 > max) n2 = max;
            tmemcpy(buf, resp, n2);
            return n2;
        }
        if (blen > max) blen = max;
        tmemcpy(buf, resp + hdr, blen);
        return blen;
    }
    return -1;
}
