#include "http.h"

/* See http.h for why this file exists at all. */

static int hstrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Case-insensitive search. Header names arrive in whatever case the server
 * felt like sending. */
static int ci_find(const uint8_t *h, int len, const char *needle) {
    int nl = hstrlen(needle);
    for (int i = 0; i + nl <= len; i++) {
        int j = 0;
        while (j < nl && lower(h[i + j]) == lower((unsigned char)needle[j])) j++;
        if (j == nl) return i;
    }
    return -1;
}

int http_header_token(const uint8_t *h, int len, const char *name,
                      const char *tok) {
    int i = ci_find(h, len, name);
    if (i < 0) return 0;
    int start = i + 1, end = start;              /* past the anchoring newline */
    while (end < len && h[end] != '\r' && h[end] != '\n') end++;
    return ci_find(h + start, end - start, tok) >= 0;
}

int http_is_chunked(const uint8_t *h, int len) {
    return http_header_token(h, len, "\ntransfer-encoding:", "chunked");
}

int http_content_length(const uint8_t *h, int len) {
    int i = ci_find(h, len, "\ncontent-length:");
    if (i < 0) return -1;
    i += 16;
    while (i < len && (h[i] == ' ' || h[i] == '\t')) i++;
    int n = 0, d = 0;
    while (i < len && h[i] >= '0' && h[i] <= '9') {
        n = n * 10 + (h[i] - '0'); i++; d++;
    }
    return d ? n : -1;
}

int http_status(const uint8_t *h, int len) {
    if (len < 12 || h[0] != 'H') return 0;
    int i = 0;
    while (i < len && h[i] != ' ') i++;
    i++;
    int n = 0, d = 0;
    while (i < len && h[i] >= '0' && h[i] <= '9') {
        n = n * 10 + (h[i] - '0'); i++; d++;
    }
    return d ? n : 0;
}

int http_keeps_alive(const uint8_t *h, int len) {
    int v11 = len > 8 && h[5] == '1' && h[6] == '.' && h[7] == '1';
    if (http_header_token(h, len, "\nconnection:", "close")) return 0;
    if (http_header_token(h, len, "\nconnection:", "keep-alive")) return 1;
    return v11;
}

/* One step of the framing: read the hex length at `in`, leave `in` just past
 * the CRLF that follows it. Returns the length, or -1 if it is not all here
 * yet -- which is the only difference between walking and decoding. */
static int chunk_size(const uint8_t *p, int len, int *in) {
    int n = 0, digits = 0;
    while (*in < len) {
        int c = p[*in];
        int v = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) break;
        n = n * 16 + v;
        (*in)++; digits++;
    }
    if (!digits) return -1;
    /* A chunk extension may sit between the length and the CRLF. */
    while (*in < len && p[*in] != '\n') (*in)++;
    if (*in >= len) return -1;
    (*in)++;
    return n;
}

int http_chunked_complete(const uint8_t *p, int len) {
    int in = 0;
    for (;;) {
        int n = chunk_size(p, len, &in);
        if (n < 0) return 0;                    /* the length is not here yet */
        if (n == 0) return 1;                   /* the end; trailers ignored  */
        in += n + 2;                            /* the data, then its CRLF    */
        if (in > len) return 0;
    }
}

int http_dechunk(uint8_t *p, int len) {
    int in = 0, out = 0;
    for (;;) {
        int n = chunk_size(p, len, &in);
        if (n <= 0) break;
        if (in + n > len) n = len - in;
        if (n <= 0) break;
        for (int i = 0; i < n; i++) p[out + i] = p[in + i];
        out += n; in += n;
        if (in < len && p[in] == '\r') in++;
        if (in < len && p[in] == '\n') in++;
    }
    return out;
}

void http_mark_dechunked(uint8_t *h, int len) {
    /* Anchored to the newline, like every other lookup here, and the width is
     * the point: seventeen characters replaced by seventeen, so not a byte
     * below moves. */
    static const char was[] = "\ntransfer-encoding:";
    static const char now[] = "X-Xyuos-Dechunked";
    int i = ci_find(h, len, was);
    if (i < 0) return;
    for (int k = 0; k < 17; k++) h[i + 1 + k] = (uint8_t)now[k];
}
