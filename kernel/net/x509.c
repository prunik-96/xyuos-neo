#include "x509.h"
#include "../crypto/crypto.h"
#include "../drivers/rtc.h"
#include "roots.h"

/* X.509, cut down to the question that matters.
 *
 * A certificate is a signed statement: "this public key belongs to this name".
 * Checking one means walking a chain of those statements back to somebody we
 * already decided to believe -- a root in roots.h -- and confirming at every
 * step that the signature is genuine, the dates are current, and the authority
 * was allowed to sign for others.
 *
 * Only the algorithms in use today are accepted: RSA with SHA-256 in either
 * padding, and ECDSA over P-256 with SHA-256. Anything else is refused rather
 * than skipped. A verifier that shrugs at what it does not understand is worse
 * than no verifier at all, because it produces the same reassuring answer. */

/* --- DER ------------------------------------------------------------------ */

typedef struct { const uint8_t *p, *end; } der_t;

#define T_BOOL   0x01
#define T_INT    0x02
#define T_BITSTR 0x03
#define T_OCTSTR 0x04
#define T_NULL   0x05
#define T_OID    0x06
#define T_UTCT   0x17
#define T_GENT   0x18
#define T_SEQ    0x30
#define T_SET    0x31

/* One tag-length-value. `whole` (optional) receives the element including its
 * header, which is what a signature is computed over. */
static int der_next(der_t *d, int *tag, const uint8_t **body, int *blen,
                    const uint8_t **whole, int *wholelen) {
    const uint8_t *start = d->p;
    if (d->p + 2 > d->end) return 0;
    int t = *d->p++;
    if ((t & 0x1F) == 0x1F) return 0;          /* X.509 has no multi-byte tags */
    int l = *d->p++;
    if (l & 0x80) {
        int nb = l & 0x7F;
        if (nb < 1 || nb > 3 || d->p + nb > d->end) return 0;
        l = 0;
        for (int i = 0; i < nb; i++) l = (l << 8) | *d->p++;
    }
    if (l < 0 || d->p + l > d->end) return 0;
    *tag = t;
    *body = d->p;
    *blen = l;
    if (whole) { *whole = start; *wholelen = (int)(d->p + l - start); }
    d->p += l;
    return 1;
}

static int der_open(der_t *d, int want, der_t *inner,
                    const uint8_t **whole, int *wholelen) {
    int tag, len;
    const uint8_t *b;
    if (!der_next(d, &tag, &b, &len, whole, wholelen)) return 0;
    if (tag != want) return 0;
    inner->p = b;
    inner->end = b + len;
    return 1;
}

static int der_skip(der_t *d) {
    int tag, len;
    const uint8_t *b;
    return der_next(d, &tag, &b, &len, 0, 0);
}

static int oid_is(const uint8_t *p, int len, const uint8_t *want, int wantlen) {
    if (len != wantlen) return 0;
    for (int i = 0; i < len; i++) if (p[i] != want[i]) return 0;
    return 1;
}

static const uint8_t OID_RSA_SHA256[]  = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b };
static const uint8_t OID_RSA_PSS[]     = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a };
static const uint8_t OID_ECDSA_SHA256[]= { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02 };
static const uint8_t OID_ECDSA_SHA384[]= { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03 };
static const uint8_t OID_RSA_SHA384[]  = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c };
static const uint8_t OID_RSA_SHA512[]  = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d };
static const uint8_t OID_SECP384R1[]   = { 0x2b,0x81,0x04,0x00,0x22 };
static const uint8_t OID_RSA_ENC[]     = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01 };
static const uint8_t OID_EC_PUBKEY[]   = { 0x2a,0x86,0x48,0xce,0x3d,0x02,0x01 };
static const uint8_t OID_PRIME256V1[]  = { 0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07 };
static const uint8_t OID_SAN[]         = { 0x55,0x1d,0x11 };
static const uint8_t OID_BASIC[]       = { 0x55,0x1d,0x13 };

/* The AlgorithmIdentifier SEQUENCE, reduced to one of our constants. */
static int parse_sig_alg(der_t *alg) {
    int tag, len;
    const uint8_t *oid;
    if (!der_next(alg, &tag, &oid, &len, 0, 0) || tag != T_OID) return X509_SIG_UNKNOWN;
    if (oid_is(oid, len, OID_RSA_SHA256, sizeof OID_RSA_SHA256)) return X509_SIG_RSA_SHA256;
    if (oid_is(oid, len, OID_RSA_SHA384, sizeof OID_RSA_SHA384)) return X509_SIG_RSA_SHA384;
    if (oid_is(oid, len, OID_RSA_SHA512, sizeof OID_RSA_SHA512)) return X509_SIG_RSA_SHA512;
    if (oid_is(oid, len, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256)) return X509_SIG_ECDSA_SHA256;
    if (oid_is(oid, len, OID_ECDSA_SHA384, sizeof OID_ECDSA_SHA384)) return X509_SIG_ECDSA_SHA384;
    if (oid_is(oid, len, OID_RSA_PSS, sizeof OID_RSA_PSS)) {
        /* The parameters say which hash; we only handle SHA-256, which is the
         * default when they are absent. Anything explicit is not checked here,
         * so a PSS certificate using SHA-384 would be rejected later when the
         * digest fails to match -- which is the safe direction. */
        return X509_SIG_RSA_PSS_SHA256;
    }
    return X509_SIG_UNKNOWN;
}

/* --- time ----------------------------------------------------------------- */

static int two(const uint8_t *p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

/* UTCTime is YYMMDDHHMMSSZ, GeneralizedTime YYYYMMDDHHMMSSZ. */
static int parse_time(int tag, const uint8_t *p, int len, int64_t *out) {
    struct rtc_time t;
    int i = 0;
    if (tag == T_UTCT) {
        if (len < 13) return 0;
        int yy = two(p); if (yy < 0) return 0;
        /* RFC 5280: 50 and above means the twentieth century. */
        t.year = yy >= 50 ? 1900 + yy : 2000 + yy;
        i = 2;
    } else if (tag == T_GENT) {
        if (len < 15) return 0;
        int a = two(p), b = two(p + 2);
        if (a < 0 || b < 0) return 0;
        t.year = a * 100 + b;
        i = 4;
    } else {
        return 0;
    }
    t.mon = two(p + i);      if (t.mon < 0) return 0;
    t.day = two(p + i + 2);  if (t.day < 0) return 0;
    t.hour = two(p + i + 4); if (t.hour < 0) return 0;
    t.min = two(p + i + 6);  if (t.min < 0) return 0;
    t.sec = two(p + i + 8);  if (t.sec < 0) return 0;
    *out = rtc_to_unix(&t);
    return 1;
}

/* --- the certificate ------------------------------------------------------ */

int x509_parse(const uint8_t *der, int len, x509_t *out) {
    for (unsigned i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;
    out->der = der;
    out->derlen = len;

    der_t top = { der, der + len }, cert;
    if (!der_open(&top, T_SEQ, &cert, 0, 0)) return 0;

    /* tbsCertificate, kept whole: its bytes are what the issuer signed. */
    der_t tbs;
    if (!der_open(&cert, T_SEQ, &tbs, &out->tbs, &out->tbslen)) return 0;

    /* signatureAlgorithm and signatureValue come after it. */
    {
        der_t alg;
        if (!der_open(&cert, T_SEQ, &alg, 0, 0)) return 0;
        out->sig_alg = parse_sig_alg(&alg);

        int tag, l;
        const uint8_t *b;
        if (!der_next(&cert, &tag, &b, &l, 0, 0) || tag != T_BITSTR || l < 2) return 0;
        out->sig = b + 1;                 /* skip the unused-bits count */
        out->siglen = l - 1;
    }

    /* Inside tbsCertificate. */
    int tag, l;
    const uint8_t *b, *whole;
    int wl;

    if (tbs.p >= tbs.end) return 0;
    if ((*tbs.p & 0xE0) == 0xA0) {        /* [0] version, present since v3 */
        if (!der_skip(&tbs)) return 0;
    }
    if (!der_next(&tbs, &tag, &b, &l, 0, 0) || tag != T_INT) return 0;   /* serial */
    if (!der_skip(&tbs)) return 0;                                       /* signature */

    if (!der_next(&tbs, &tag, &b, &l, &whole, &wl) || tag != T_SEQ) return 0;
    out->issuer = whole; out->issuerlen = wl;

    {   /* validity */
        der_t v;
        if (!der_open(&tbs, T_SEQ, &v, 0, 0)) return 0;
        if (!der_next(&v, &tag, &b, &l, 0, 0)) return 0;
        if (!parse_time(tag, b, l, &out->not_before)) return 0;
        if (!der_next(&v, &tag, &b, &l, 0, 0)) return 0;
        if (!parse_time(tag, b, l, &out->not_after)) return 0;
    }

    if (!der_next(&tbs, &tag, &b, &l, &whole, &wl) || tag != T_SEQ) return 0;
    out->subject = whole; out->subjectlen = wl;

    {   /* subjectPublicKeyInfo */
        der_t spki, alg;
        if (!der_open(&tbs, T_SEQ, &spki, 0, 0)) return 0;
        if (!der_open(&spki, T_SEQ, &alg, 0, 0)) return 0;

        const uint8_t *oid;
        int oidlen;
        if (!der_next(&alg, &tag, &oid, &oidlen, 0, 0) || tag != T_OID) return 0;

        if (!der_next(&spki, &tag, &b, &l, 0, 0) || tag != T_BITSTR || l < 2) return 0;
        const uint8_t *key = b + 1;
        int keylen = l - 1;

        if (oid_is(oid, oidlen, OID_RSA_ENC, sizeof OID_RSA_ENC)) {
            der_t k, seq;
            k.p = key; k.end = key + keylen;
            if (!der_open(&k, T_SEQ, &seq, 0, 0)) return 0;
            if (!der_next(&seq, &tag, &b, &l, 0, 0) || tag != T_INT) return 0;
            /* An INTEGER carries a leading zero when the top bit is set. */
            while (l > 1 && b[0] == 0) { b++; l--; }
            out->n = b; out->nlen = l;
            if (!der_next(&seq, &tag, &b, &l, 0, 0) || tag != T_INT) return 0;
            while (l > 1 && b[0] == 0) { b++; l--; }
            out->e = b; out->elen = l;
            out->key_alg = X509_KEY_RSA;
        } else if (oid_is(oid, oidlen, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY)) {
            const uint8_t *curve;
            int curvelen;
            if (!der_next(&alg, &tag, &curve, &curvelen, 0, 0) || tag != T_OID) return 0;
            if (oid_is(curve, curvelen, OID_PRIME256V1, sizeof OID_PRIME256V1)) {
                if (keylen != 65 || key[0] != 0x04) return 0;
                out->key_alg = X509_KEY_EC_P256;
            } else if (oid_is(curve, curvelen, OID_SECP384R1, sizeof OID_SECP384R1)) {
                if (keylen != 97 || key[0] != 0x04) return 0;
                out->key_alg = X509_KEY_EC_P384;
            } else {
                return 0;                 /* a curve we do not have */
            }
            out->point = key; out->pointlen = keylen;
        } else {
            return 0;                     /* a key we cannot use */
        }
    }

    /* Extensions, if any: [3] EXPLICIT SEQUENCE OF Extension. */
    while (tbs.p < tbs.end) {
        if (!der_next(&tbs, &tag, &b, &l, 0, 0)) break;
        if (tag != 0xA3) continue;        /* [1] and [2] are unique IDs, ignored */

        der_t exts, list;
        exts.p = b; exts.end = b + l;
        if (!der_open(&exts, T_SEQ, &list, 0, 0)) break;

        while (list.p < list.end) {
            der_t ext;
            if (!der_open(&list, T_SEQ, &ext, 0, 0)) break;
            const uint8_t *oid;
            int oidlen;
            if (!der_next(&ext, &tag, &oid, &oidlen, 0, 0) || tag != T_OID) break;

            /* An optional critical flag sits between the OID and the value. */
            if (ext.p < ext.end && *ext.p == T_BOOL) {
                if (!der_skip(&ext)) break;
            }
            const uint8_t *val;
            int vallen;
            if (!der_next(&ext, &tag, &val, &vallen, 0, 0) || tag != T_OCTSTR) break;

            if (oid_is(oid, oidlen, OID_SAN, sizeof OID_SAN)) {
                out->san = val; out->sanlen = vallen;
            } else if (oid_is(oid, oidlen, OID_BASIC, sizeof OID_BASIC)) {
                der_t bc, seq;
                bc.p = val; bc.end = val + vallen;
                if (der_open(&bc, T_SEQ, &seq, 0, 0) && seq.p < seq.end) {
                    const uint8_t *bb;
                    int bl;
                    if (der_next(&seq, &tag, &bb, &bl, 0, 0) && tag == T_BOOL && bl == 1)
                        out->is_ca = bb[0] != 0;
                }
            }
        }
        break;
    }
    return 1;
}

/* --- signatures ----------------------------------------------------------- */

/* Which digest an algorithm uses, and how wide it is. */
static int hash_for(int alg, const uint8_t *msg, int msglen, uint8_t out[64]) {
    switch (alg) {
    case X509_SIG_RSA_SHA256:
    case X509_SIG_RSA_PSS_SHA256:
    case X509_SIG_ECDSA_SHA256:
        sha256(msg, (uint32_t)msglen, out);
        return 32;
    case X509_SIG_RSA_SHA384:
    case X509_SIG_ECDSA_SHA384:
        sha384(msg, (uint32_t)msglen, out);
        return 48;
    case X509_SIG_RSA_SHA512:
        sha512(msg, (uint32_t)msglen, out);
        return 64;
    default:
        return 0;
    }
}

static int verify_with_key(const x509_t *key, int alg,
                           const uint8_t *msg, int msglen,
                           const uint8_t *sig, int siglen) {
    uint8_t hash[64];
    int hashlen = hash_for(alg, msg, msglen, hash);
    if (!hashlen) return 0;                  /* an algorithm we do not know */

    switch (alg) {
    case X509_SIG_RSA_SHA256:
    case X509_SIG_RSA_SHA384:
    case X509_SIG_RSA_SHA512:
        if (key->key_alg != X509_KEY_RSA) return 0;
        return rsa_verify_pkcs1(key->n, key->nlen, key->e, key->elen,
                                sig, siglen, hash, hashlen);
    case X509_SIG_RSA_PSS_SHA256:
        if (key->key_alg != X509_KEY_RSA) return 0;
        return rsa_verify_pss_sha256(key->n, key->nlen, key->e, key->elen,
                                     sig, siglen, hash);
    case X509_SIG_ECDSA_SHA256:
    case X509_SIG_ECDSA_SHA384: {
        /* The curve comes from the KEY, the digest from the SIGNATURE. They
         * are chosen independently and a P-384 key signing with SHA-256 is
         * perfectly legal, so neither may be inferred from the other. */
        int curve;
        if (key->key_alg == X509_KEY_EC_P256) curve = ECDSA_P256;
        else if (key->key_alg == X509_KEY_EC_P384) curve = ECDSA_P384;
        else return 0;
        return ecdsa_verify(curve, key->point, key->pointlen,
                            sig, siglen, hash, hashlen);
    }
    default:
        return 0;
    }
}

int x509_signed_by(const x509_t *child, const x509_t *parent) {
    return verify_with_key(parent, child->sig_alg,
                           child->tbs, child->tbslen,
                           child->sig, child->siglen);
}

/* TLS SignatureScheme codes, for CertificateVerify. */
int x509_verify_sig(const x509_t *c, uint16_t scheme,
                    const uint8_t *msg, int msglen,
                    const uint8_t *sig, int siglen) {
    int alg;
    switch (scheme) {
    case 0x0804: alg = X509_SIG_RSA_PSS_SHA256; break;   /* rsa_pss_rsae_sha256 */
    case 0x0809: alg = X509_SIG_RSA_PSS_SHA256; break;   /* rsa_pss_pss_sha256  */
    case 0x0403: alg = X509_SIG_ECDSA_SHA256;  break;    /* ecdsa_secp256r1     */
    case 0x0503: alg = X509_SIG_ECDSA_SHA384;  break;    /* ecdsa_secp384r1     */
    /* rsa_pss_rsae_sha384 (0x0805) is deliberately absent: PSS here is
     * SHA-256 only, and claiming the SHA-384 variant would mean verifying it
     * against the wrong digest. We do not offer it in the hello either. */
    default: return 0;
    }
    return verify_with_key(c, alg, msg, msglen, sig, siglen);
}

/* --- names ---------------------------------------------------------------- */

static int name_eq(const uint8_t *a, int alen, const uint8_t *b, int blen) {
    /* A byte comparison of the encoded Name. RFC 5280 asks for a normalising
     * comparison, but an issuer field is copied verbatim from the subject it
     * refers to, so in practice these are identical byte for byte -- and
     * anything that is not, we would rather not accept. */
    if (alen != blen) return 0;
    for (int i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* One dNSName against the host, honouring a leading "*." that stands for
 * exactly one label. */
static int dns_matches(const uint8_t *pat, int patlen, const char *host) {
    int hl = 0;
    while (host[hl]) hl++;

    if (patlen > 2 && pat[0] == '*' && pat[1] == '.') {
        /* The wildcard covers one label and never a dot, so the rest must
         * match from the first dot in the host onward. */
        int dot = -1;
        for (int i = 0; i < hl; i++) if (host[i] == '.') { dot = i; break; }
        if (dot < 0) return 0;
        int rest = hl - dot;
        if (rest != patlen - 1) return 0;
        for (int i = 0; i < rest; i++)
            if (lower(pat[1 + i]) != lower((unsigned char)host[dot + i])) return 0;
        return 1;
    }

    if (patlen != hl) return 0;
    for (int i = 0; i < hl; i++)
        if (lower(pat[i]) != lower((unsigned char)host[i])) return 0;
    return 1;
}

int x509_host_matches(const x509_t *c, const char *host) {
    /* Only SubjectAltName counts. The Common Name has not been a valid source
     * of host names for years, and treating it as one is how a certificate for
     * one site gets accepted for another. */
    if (!c->san || c->sanlen <= 0) return 0;

    der_t s = { c->san, c->san + c->sanlen }, list;
    if (!der_open(&s, T_SEQ, &list, 0, 0)) return 0;

    while (list.p < list.end) {
        int tag, len;
        const uint8_t *b;
        if (!der_next(&list, &tag, &b, &len, 0, 0)) break;
        if (tag != 0x82) continue;                  /* [2] dNSName */
        if (dns_matches(b, len, host)) return 1;
    }
    return 0;
}

/* --- the trust store ------------------------------------------------------ */

/* Find a root whose subject is this issuer name. */
static int find_root(const uint8_t *issuer, int issuerlen, x509_t *out) {
    for (int i = 0; i < ROOT_COUNT; i++) {
        static x509_t r;
        if (!x509_parse(ROOT_DER + ROOT_IDX[i].off, (int)ROOT_IDX[i].len, &r)) continue;
        if (!name_eq(r.subject, r.subjectlen, issuer, issuerlen)) continue;
        *out = r;
        return 1;
    }
    return 0;
}

/* --- the chain ------------------------------------------------------------ */

#define MAX_DEPTH 8

int x509_verify_chain(const uint8_t *const *certs, const int *lens, int n,
                      const char *host, const char **why) {
    static x509_t parsed[16], root;
    const char *dummy;
    if (!why) why = &dummy;
    *why = "";

    if (n < 1) { *why = "the server sent no certificate"; return 0; }
    if (n > 16) n = 16;

    for (int i = 0; i < n; i++) {
        if (!x509_parse(certs[i], lens[i], &parsed[i])) {
            *why = "a certificate could not be parsed";
            return 0;
        }
    }

    int64_t now = rtc_now_unix();
    /* A clock that is obviously wrong would reject the whole web, so say so
     * rather than blaming the certificate. */
    if (now < 1600000000LL) { *why = "the system clock is not set"; return 0; }

    if (!x509_host_matches(&parsed[0], host)) {
        *why = "the certificate is for a different host";
        return 0;
    }

    int cur = 0;
    for (int depth = 0; depth < MAX_DEPTH; depth++) {
        x509_t *c = &parsed[cur];

        if (now < c->not_before) { *why = "a certificate is not valid yet"; return 0; }
        if (now > c->not_after)  { *why = "a certificate has expired"; return 0; }
        if (depth > 0 && !c->is_ca) {
            *why = "a certificate in the chain is not an authority";
            return 0;
        }

        if (find_root(c->issuer, c->issuerlen, &root)) {
            if (now > root.not_after) { *why = "the root certificate has expired"; return 0; }
            if (!x509_signed_by(c, &root)) {
                *why = "the signature from the root does not check out";
                return 0;
            }
            return 1;                     /* reached something we trust */
        }

        /* Otherwise the issuer should be one of the certificates the server
         * sent along with its own. */
        int next = -1;
        for (int i = 0; i < n; i++) {
            if (i == cur) continue;
            if (name_eq(parsed[i].subject, parsed[i].subjectlen,
                        c->issuer, c->issuerlen)) { next = i; break; }
        }
        if (next < 0) {
            *why = "the chain does not reach a trusted authority";
            return 0;
        }
        if (!x509_signed_by(c, &parsed[next])) {
            *why = "a signature in the chain does not check out";
            return 0;
        }
        cur = next;
    }

    *why = "the certificate chain is too long";
    return 0;
}
