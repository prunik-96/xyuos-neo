/* iconv, joined in the middle out of libparserutils' codecs.
 * See include/iconv.h for what this is and why it is strict.
 *
 * The whole difficulty here is one habit these codecs have. Handed an output
 * buffer with no room left, they do not simply refuse the character: they
 * take it, keep it in a buffer of their own, advance the source pointer past
 * it, and report that they wanted more space. Both halves do it -- the
 * decoder with its read_buf, the encoder with its write_buf.
 *
 * So "no bytes came out" does not mean "no bytes went in", and code that
 * assumes otherwise walks its input pointer past characters that are sitting
 * inside the codec. It loses one at every buffer boundary and one more at
 * the end of the text, which is the sort of fault that looks like a rare
 * encoding bug for years.
 *
 * Two things follow, and they are the shape of everything below:
 *
 *   Characters are decoded a batch at a time into `ucs`, and the caller's
 *   input pointer does not move until every character of that batch has been
 *   written out. The codec may hold one back; the next call drains it,
 *   because these decoders flush what they are holding before looking at new
 *   input -- which is why the decode below is still called when there is no
 *   input left.
 *
 *   Each character is encoded into `pend`, which is wide enough for any one
 *   character in any encoding here. The encoder therefore never runs out of
 *   room and never stashes anything. Whether the result fits in the caller's
 *   buffer is then a question this code answers itself.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include <parserutils/errors.h>
#include <parserutils/charset/codec.h>

#include "iconv.h"

/* Room for one character in any encoding these codecs write. The longest is
 * four bytes; the rest is so that filling it is impossible rather than
 * merely unlikely. */
#define ONE_CHAR 16

/* How many characters are decoded at a time. Any number works; a larger one
 * means fewer calls into the decoder. */
#define BATCH 64

struct conv {
    parserutils_charset_codec *from;   /* source encoding <-> UCS-4 */
    parserutils_charset_codec *to;     /* UCS-4 <-> destination encoding */

    /* Characters decoded but not yet all written out. Stored the way the
     * codecs read and write them, which is UCS-4 big-endian. */
    uint32_t ucs[BATCH];
    size_t   nucs, ucspos;
    size_t   consumed;      /* input bytes this batch came from, not yet
                               charged to the caller's pointer */

    /* One character, encoded, waiting for room in the caller's buffer. */
    unsigned char pend[ONE_CHAR];
    size_t        npend;
};

/* "UTF-8//TRANSLIT" names UTF-8. What follows the slashes asks for a
 * particular way of failing, and this answers every such request the same
 * strict way, so the suffix is dropped rather than misread as part of the
 * name. */
static int plain_name(const char *in, char *out, size_t max) {
    if (!in) return 0;
    size_t n = 0;
    while (in[n] && n + 1 < max) {
        if (in[n] == '/' && in[n + 1] == '/') break;
        n++;
    }
    if (n == 0) return 0;
    if (in[n] != 0 && !(in[n] == '/' && in[n + 1] == '/'))
        return 0;                               /* ran out of room */
    memcpy(out, in, n);
    out[n] = 0;
    return 1;
}

static parserutils_charset_codec *make(const char *name) {
    char plain[64];
    if (!plain_name(name, plain, sizeof plain)) return NULL;

    parserutils_charset_codec *c = NULL;
    if (parserutils_charset_codec_create(plain, &c) != PARSERUTILS_OK)
        return NULL;

    /* Strict, so that an unconvertible character is reported to the caller
     * instead of being quietly replaced. See the header. */
    parserutils_charset_codec_optparams p;
    p.error_mode.mode = PARSERUTILS_CHARSET_CODEC_ERROR_STRICT;
    if (parserutils_charset_codec_setopt(c,
            PARSERUTILS_CHARSET_CODEC_ERROR_MODE, &p) != PARSERUTILS_OK) {
        /* Without this the codec substitutes rather than reports, and the
         * caller never learns that a character would not fit. A codec that
         * will not be told is no use for iconv's contract. */
        parserutils_charset_codec_destroy(c);
        return NULL;
    }
    return c;
}

iconv_t iconv_open(const char *to, const char *from) {
    struct conv *cv = calloc(1, sizeof *cv);
    if (!cv) { errno = ENOMEM; return (iconv_t)-1; }

    cv->from = make(from);
    cv->to   = make(to);
    if (!cv->from || !cv->to) {
        if (cv->from) parserutils_charset_codec_destroy(cv->from);
        if (cv->to)   parserutils_charset_codec_destroy(cv->to);
        free(cv);
        errno = EINVAL;                          /* no such conversion */
        return (iconv_t)-1;
    }
    return (iconv_t)cv;
}

int iconv_close(iconv_t cd) {
    struct conv *cv = (struct conv *)cd;
    if (!cv || cd == (iconv_t)-1) { errno = EBADF; return -1; }
    parserutils_charset_codec_destroy(cv->from);
    parserutils_charset_codec_destroy(cv->to);
    free(cv);
    return 0;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inleft,
             char **outbuf, size_t *outleft) {
    struct conv *cv = (struct conv *)cd;
    if (!cv || cd == (iconv_t)-1) { errno = EBADF; return (size_t)-1; }

    /* iconv(cd, NULL, ...) puts the descriptor back to its initial state. */
    if (inbuf == NULL || *inbuf == NULL) {
        parserutils_charset_codec_reset(cv->from);
        parserutils_charset_codec_reset(cv->to);
        cv->nucs = cv->ucspos = cv->consumed = cv->npend = 0;
        return 0;
    }
    if (!inleft || !outbuf || !*outbuf || !outleft) {
        errno = EINVAL;
        return (size_t)-1;
    }

    for (;;) {
        /* What is already encoded goes out first. Only when the last
         * character of a batch has gone does the caller's input pointer move
         * past the bytes that batch came from. */
        if (cv->npend > 0) {
            if (cv->npend > *outleft) { errno = E2BIG; return (size_t)-1; }
            memcpy(*outbuf, cv->pend, cv->npend);
            *outbuf  += cv->npend;
            *outleft -= cv->npend;
            cv->npend = 0;
        }
        if (cv->ucspos == cv->nucs && cv->consumed > 0) {
            *inbuf   += cv->consumed;
            *inleft  -= cv->consumed;
            cv->consumed = 0;
        }

        /* Refill. This runs with no input left as well, on purpose: a
         * decoder holding a character it could not fit last time gives it up
         * before it looks at anything new, and that character is how the
         * last one of a text gets out. */
        if (cv->ucspos == cv->nucs) {
            const uint8_t *src = (const uint8_t *)*inbuf;
            size_t srclen = *inleft;
            uint8_t *mid = (uint8_t *)cv->ucs;
            size_t midlen = sizeof cv->ucs;

            parserutils_error e =
                parserutils_charset_codec_decode(cv->from, &src, &srclen,
                                                 &mid, &midlen);

            cv->nucs = (sizeof cv->ucs - midlen) / 4;
            cv->ucspos = 0;
            cv->consumed = *inleft - srclen;

            if (cv->nucs == 0) {
                /* Nothing came out. If input went in anyway it was a byte
                 * order mark or the like: charge it and go round again. */
                if (cv->consumed > 0) continue;
                if (*inleft == 0) return 0;   /* everything is written */
                errno = (e == PARSERUTILS_NEEDDATA) ? EINVAL : EILSEQ;
                return (size_t)-1;
            }
        }

        const uint8_t *one = (const uint8_t *)&cv->ucs[cv->ucspos];
        size_t onelen = 4;
        uint8_t *dst = cv->pend;
        size_t dstlen = sizeof cv->pend;

        parserutils_error e =
            parserutils_charset_codec_encode(cv->to, &one, &onelen,
                                             &dst, &dstlen);
        if (onelen != 0) {
            /* Scratch cannot be full, so this is the destination encoding
             * having no way to write the character. */
            errno = EILSEQ;
            (void)e;
            return (size_t)-1;
        }

        cv->npend = sizeof cv->pend - dstlen;
        cv->ucspos++;
    }
}
