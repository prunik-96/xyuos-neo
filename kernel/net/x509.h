#ifndef X509_H
#define X509_H

#include <stdint.h>

/* Certificates: enough of X.509 to answer one question honestly -- is the
 * machine on the other end of this connection the one whose name we asked for?
 *
 * Everything here points INTO the DER it was given. Nothing is copied, so the
 * certificate bytes must outlive the parsed form. */

enum {
    X509_SIG_UNKNOWN = 0,
    X509_SIG_RSA_SHA256,        /* sha256WithRSAEncryption */
    X509_SIG_RSA_SHA384,
    X509_SIG_RSA_SHA512,
    X509_SIG_RSA_PSS_SHA256,    /* rsassaPss with SHA-256 */
    X509_SIG_ECDSA_SHA256,      /* ecdsa-with-SHA256 */
    X509_SIG_ECDSA_SHA384,
};

enum {
    X509_KEY_UNKNOWN = 0,
    X509_KEY_RSA,
    X509_KEY_EC_P256,
    X509_KEY_EC_P384,
};

typedef struct {
    const uint8_t *der;     int derlen;
    const uint8_t *tbs;     int tbslen;      /* exactly what the signature covers */
    int            sig_alg;
    const uint8_t *sig;     int siglen;
    const uint8_t *issuer;  int issuerlen;   /* raw DER of the Name */
    const uint8_t *subject; int subjectlen;

    int            key_alg;
    const uint8_t *n;       int nlen;        /* RSA modulus  */
    const uint8_t *e;       int elen;        /* RSA exponent */
    const uint8_t *point;   int pointlen;    /* EC public point, 04||X||Y */

    const uint8_t *san;     int sanlen;      /* SubjectAltName contents, if any */
    int            is_ca;
    int64_t        not_before, not_after;    /* seconds since 1970 */
} x509_t;

/* 1 on success. Rejects anything it does not fully understand. */
int x509_parse(const uint8_t *der, int len, x509_t *out);

/* 1 if `child` really was signed by `parent`'s key. */
int x509_signed_by(const x509_t *child, const x509_t *parent);

/* 1 if the certificate was issued for this host name (SubjectAltName, with
 * a leading *. matching exactly one label). */
int x509_host_matches(const x509_t *c, const char *host);

/* Verify a chain as TLS presents it: certs[0] is the server's, the rest are
 * whatever it sent to connect that to a root. Returns 1 only if every link is
 * signed, in date, and ends at a root this system trusts. On failure *why is
 * set to a short reason. */
int x509_verify_chain(const uint8_t *const *certs, const int *lens, int n,
                      const char *host, const char **why);

/* Verify a signature made by the certificate's own key, with the algorithm
 * identified by a TLS SignatureScheme code. Used for CertificateVerify. */
int x509_verify_sig(const x509_t *c, uint16_t scheme,
                    const uint8_t *msg, int msglen,
                    const uint8_t *sig, int siglen);

#endif
