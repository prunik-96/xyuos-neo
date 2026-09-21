/* zlib's inflate side, over userland/inflate.h. See include/zlib.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "inflate.h"      /* inflate_raw, gzip_body_at, zlib_body_at */
#include "zlib.h"

/* --- the memory side ---------------------------------------------------- */

struct z_state {
    unsigned char *out;      /* the whole result, held until drained */
    unsigned long  len, pos;
    int            windowBits;
    int            taken;    /* the input has been read and inflated */
};

/* Which wrapper the bytes are wearing, given what windowBits asked for.
 * Returns the offset of the DEFLATE stream, or -1 if the bytes do not match
 * what was asked for. */
static int body_offset(int windowBits, const unsigned char *p, int n) {
    if (windowBits < 0) return 0;                  /* raw, no wrapper */

    if (windowBits >= 32) {                        /* work it out */
        int at = gzip_body_at(p, n);
        if (at >= 0) return at;
        return zlib_body_at(p, n);                 /* 2, or 0 for bare */
    }
    if (windowBits >= 16) return gzip_body_at(p, n);
    return zlib_body_at(p, n);
}

int inflateInit2(z_streamp strm, int windowBits) {
    if (!strm) return Z_STREAM_ERROR;

    struct z_state *st = calloc(1, sizeof *st);
    if (!st) return Z_MEM_ERROR;

    st->windowBits = windowBits;
    strm->state = st;
    strm->msg = NULL;
    strm->total_in = strm->total_out = 0;
    return Z_OK;
}

int inflateInit(z_streamp strm) { return inflateInit2(strm, MAX_WBITS); }

int inflateReset(z_streamp strm) {
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    free(strm->state->out);
    strm->state->out = NULL;
    strm->state->len = strm->state->pos = 0;
    strm->state->taken = 0;
    strm->total_in = strm->total_out = 0;
    strm->msg = NULL;
    return Z_OK;
}

int inflate(z_streamp strm, int flush) {
    (void)flush;
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    struct z_state *st = strm->state;

    /* First call: take everything offered as the whole stream. A caller
     * arriving later with more input is one this cannot serve, and is told
     * so rather than given a short answer. */
    if (!st->taken) {
        if (strm->avail_in == 0) {
            strm->msg = "no input";
            return Z_BUF_ERROR;
        }
        const unsigned char *p = strm->next_in;
        int n = (int)strm->avail_in;

        int at = body_offset(st->windowBits, p, n);
        if (at < 0 || at >= n) {
            strm->msg = "not a stream of the kind asked for";
            return Z_DATA_ERROR;
        }

        unsigned long got = 0;
        st->out = inflate_raw(p + at, (unsigned long)(n - at), &got);
        if (!st->out) {
            strm->msg = "the compressed data does not decode";
            return Z_DATA_ERROR;
        }
        st->len = got;
        st->pos = 0;
        st->taken = 1;

        strm->next_in  += strm->avail_in;
        strm->total_in += strm->avail_in;
        strm->avail_in  = 0;
    } else if (strm->avail_in != 0) {
        strm->msg = "this inflate takes its input all at once";
        return Z_DATA_ERROR;
    }

    unsigned long left = st->len - st->pos;
    if (left == 0) return Z_STREAM_END;
    if (strm->avail_out == 0) {
        strm->msg = "no room to put anything";
        return Z_BUF_ERROR;
    }

    unsigned long take = left;
    if (take > strm->avail_out) take = strm->avail_out;

    memcpy(strm->next_out, st->out + st->pos, (size_t)take);
    st->pos         += take;
    strm->next_out  += take;
    strm->avail_out -= (uInt)take;
    strm->total_out += take;

    return (st->pos == st->len) ? Z_STREAM_END : Z_OK;
}

int inflateEnd(z_streamp strm) {
    if (!strm) return Z_STREAM_ERROR;
    if (strm->state) {
        free(strm->state->out);
        free(strm->state);
        strm->state = NULL;
    }
    return Z_OK;
}

const char *zlibVersion(void) { return ZLIB_VERSION; }

const char *zError(int err) {
    switch (err) {
    case Z_OK:            return "";
    case Z_STREAM_END:    return "end of stream";
    case Z_NEED_DICT:     return "need dictionary";
    case Z_ERRNO:         return "file error";
    case Z_STREAM_ERROR:  return "stream error";
    case Z_DATA_ERROR:    return "data error";
    case Z_MEM_ERROR:     return "insufficient memory";
    case Z_BUF_ERROR:     return "buffer error";
    case Z_VERSION_ERROR: return "incompatible version";
    default:              return "unknown error";
    }
}

/* --- the file side ------------------------------------------------------ */

struct gz_file {
    unsigned char *data;     /* the file, uncompressed */
    unsigned long  len, pos;
};

gzFile gzopen(const char *path, const char *mode) {
    if (!path) { errno = EINVAL; return NULL; }
    /* Reading only. A write mode is refused here rather than at the first
     * write, so the caller finds out while it still has the file name. */
    for (const char *m = mode; m && *m; m++)
        if (*m == 'w' || *m == 'a') { errno = EINVAL; return NULL; }

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);

    unsigned char *raw = malloc((size_t)size + 1);
    if (!raw) { fclose(f); errno = ENOMEM; return NULL; }

    size_t got = fread(raw, 1, (size_t)size, f);
    fclose(f);
    raw[got] = 0;

    struct gz_file *g = calloc(1, sizeof *g);
    if (!g) { free(raw); errno = ENOMEM; return NULL; }

    /* Gzipped or not: a catalogue that was never compressed reads through
     * unchanged, which is how zlib's own gzopen behaves. */
    int at = gzip_body_at(raw, (int)got);
    if (at >= 0 && at < (int)got) {
        unsigned long outlen = 0;
        unsigned char *out = inflate_raw(raw + at, got - (unsigned long)at,
                                         &outlen);
        free(raw);
        if (!out) { free(g); errno = EILSEQ; return NULL; }
        g->data = out;
        g->len  = outlen;
    } else {
        g->data = raw;
        g->len  = got;
    }
    return g;
}

char *gzgets(gzFile f, char *buf, int len) {
    if (!f || !buf || len <= 0) { errno = EINVAL; return NULL; }
    if (f->pos >= f->len) return NULL;             /* end of file */

    int i = 0;
    while (i < len - 1 && f->pos < f->len) {
        unsigned char c = f->data[f->pos++];
        buf[i++] = (char)c;
        if (c == '\n') break;                      /* kept, as zlib keeps it */
    }
    buf[i] = 0;
    return buf;
}

int gzread(gzFile f, void *buf, unsigned len) {
    if (!f || !buf) { errno = EINVAL; return -1; }
    unsigned long left = f->len - f->pos;
    if (left > len) left = len;
    memcpy(buf, f->data + f->pos, (size_t)left);
    f->pos += left;
    return (int)left;
}

int gzeof(gzFile f) { return f ? (f->pos >= f->len) : 1; }

int gzclose(gzFile f) {
    if (!f) return Z_STREAM_ERROR;
    free(f->data);
    free(f);
    return Z_OK;
}
