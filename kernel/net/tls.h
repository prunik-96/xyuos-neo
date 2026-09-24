#ifndef TLS_H
#define TLS_H

#include <stdint.h>

/* A TLS 1.3 client, and only a client.
 *
 * One connection at a time, on top of the single TCP connection in net.c, with
 * one cipher suite (TLS_AES_128_GCM_SHA256) and one key exchange group
 * (X25519). Those are the ones RFC 8446 requires every implementation to
 * support, so a client that speaks exactly them can reach every server -- and
 * a client that supported more would only be more code to get wrong.
 *
 * The server's certificate IS checked: the chain is built and verified against
 * the trust store in roots.h, the name is matched, and the CertificateVerify
 * signature is checked against the transcript. A handshake that cannot prove
 * the identity fails rather than continuing unauthenticated. */

/* Open a TLS connection to an already-resolved address. `host` goes in the SNI
 * extension, which most servers now require to pick a certificate at all.
 * Returns 1 on success. */
int tls_connect(uint32_t ip, uint16_t port, const char *host);

/* 1 if the peer's identity was actually checked. */
int tls_verified(void);

int  tls_write(const void *data, int len);
/* Up to `max` bytes of application data. Returns the count, 0 when the peer
 * has closed cleanly, and -1 on a protocol failure. */
int  tls_read(void *buf, int max);
void tls_close(void);

/* A one-line description of why the last tls_connect() failed. */
const char *tls_error(void);

/* https GET, with the same shape as net_http_get. Consecutive requests to the
 * same host reuse one connection and skip the handshake entirely. */
int tls_https_get(const char *host, uint32_t ip, uint16_t port,
                  const char *path, char *buf, int max, int keep_headers,
                  const char *body, int blen, const char *xhdr);

/* Close the kept-open connection, if there is one. */
void tls_pool_flush(void);

#endif
