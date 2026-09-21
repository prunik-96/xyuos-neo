/* NetSurf's http and https fetcher, over xyuOS's own network syscall.
 *
 * NetSurf normally fetches with libcurl and OpenSSL. Neither is here, and
 * neither is wanted: HTTP and TLS live in this kernel already, behind three
 * calls -- begin a fetch, ask after it, take the bytes. The fetcher interface
 * is pluggable precisely so that a system can answer this question its own
 * way, and this is that answer. No part of NetSurf is patched to make it fit;
 * the frontend registers this after netsurf_init() and NetSurf uses it.
 *
 * One thing shapes everything below: THE KERNEL RUNS ONE FETCH AT A TIME.
 * net_fetch_start refuses while another is running, because there is a single
 * fetch task and a single buffer behind it. A browser asks for a page and
 * then twenty images at once, so this keeps a queue and feeds the kernel one
 * request at a time, in the order NetSurf asked. Pages come in slower than
 * they would over twenty sockets; nothing is lost, and nothing is pretended.
 *
 * The other thing worth knowing: the kernel returns the response verbatim,
 * headers and all, and it asks for gzip. So the body arrives compressed and
 * is inflated here, and the Content-Encoding header is dropped on the way
 * out -- passing it on would tell the rest of NetSurf that bytes it has
 * already been given in the clear are still compressed.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include <libwapcaplet/libwapcaplet.h>

#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/ring.h"
#include "utils/utils.h"

#include "content/fetch.h"
#include "content/fetchers.h"

#include "fetch_xyuos.h"

/* xyuOS's own calls. Down here so that NetSurf's headers see a clean
 * namespace first. */
#include <unistd.h>

#include "inflate.h"

/* What the kernel's fetch buffer holds. A response longer than this is
 * truncated there, so there is no point asking for more here. */
#define XY_MAX (1024 * 1024)

/* Headers the kernel writes into every request itself. NetSurf must not add
 * a second copy of any of them. */
static const char *const kernel_writes[] = {
    "host", "connection", "user-agent", "accept", "accept-encoding",
    "content-type", "content-length", NULL
};

struct fetch_xyuos_context {
    struct fetch *parent_fetch;
    nsurl *url;

    char  *host;
    char  *path;              /* path and query together, as sent */
    int    port;
    int    tls;

    char  *body;              /* a POST body, or NULL */
    size_t bodylen;
    char  *xhdr;              /* extra headers, each ending in CRLF */

    bool   only_2xx;
    bool   aborted;
    bool   locked;
    bool   sent;              /* handed to the kernel */
    int    reported;          /* bytes the last progress message named */

    struct fetch_xyuos_context *r_next, *r_prev;
};

static struct fetch_xyuos_context *ring = NULL;
static struct fetch_xyuos_context *inflight = NULL;
static char *rxbuf = NULL;          /* one response at a time, one buffer */

/* --- small helpers ------------------------------------------------------- */

static char *dup_component(const nsurl *url, nsurl_component part) {
    lwc_string *s = nsurl_get_component(url, part);
    if (s == NULL) return NULL;
    char *out = strdup(lwc_string_data(s));
    lwc_string_unref(s);
    return out;
}

static bool header_is_kernels(const char *line) {
    const char *colon = strchr(line, ':');
    size_t n = colon ? (size_t)(colon - line) : strlen(line);
    for (int i = 0; kernel_writes[i]; i++) {
        if (strlen(kernel_writes[i]) != n) continue;
        size_t k;
        for (k = 0; k < n; k++)
            if (tolower((unsigned char)line[k]) != kernel_writes[i][k]) break;
        if (k == n) return true;
    }
    return false;
}

static void send_msg(const fetch_msg *msg, struct fetch_xyuos_context *c) {
    c->locked = true;
    fetch_send_callback(msg, c->parent_fetch);
    c->locked = false;
}

static void send_error(struct fetch_xyuos_context *c, const char *why) {
    fetch_msg msg;
    msg.type = FETCH_ERROR;
    msg.data.error = why;
    send_msg(&msg, c);
}

/* --- the fetcher operations ---------------------------------------------- */

static bool fetch_xyuos_initialise(lwc_string *scheme) {
    NSLOG(netsurf, INFO, "xyuOS fetcher taking %s", lwc_string_data(scheme));
    if (rxbuf == NULL) {
        rxbuf = malloc(XY_MAX);
        if (rxbuf == NULL) return false;
    }
    return true;
}

static void fetch_xyuos_finalise(lwc_string *scheme) {
    (void)scheme;
    /* rxbuf is shared between the http and https registrations, so it is not
     * freed here: the other one may still be live. It is one buffer for the
     * life of the program either way. */
}

static bool fetch_xyuos_can_fetch(const nsurl *url) {
    lwc_string *host = nsurl_get_component(url, NSURL_HOST);
    bool ok = (host != NULL);
    if (host) lwc_string_unref(host);
    return ok;                 /* a name to connect to is all it needs */
}

static void *fetch_xyuos_setup(struct fetch *parent_fetch, nsurl *url,
                               bool only_2xx, bool downgrade_tls,
                               const char *post_urlenc,
                               const struct fetch_multipart_data *post_multipart,
                               const char **headers) {
    (void)downgrade_tls;       /* there is nothing to downgrade to */

    if (post_multipart != NULL) {
        /* A file upload. The kernel writes one content type for a body and
         * it is the form-encoded one, so this cannot be built honestly here
         * yet. Refusing is how NetSurf finds out. */
        NSLOG(netsurf, WARNING,
              "multipart POST is not supported by the xyuOS fetcher");
        return NULL;
    }

    struct fetch_xyuos_context *ctx = calloc(1, sizeof *ctx);
    if (ctx == NULL) return NULL;

    ctx->parent_fetch = parent_fetch;
    ctx->url = nsurl_ref(url);
    ctx->only_2xx = only_2xx;

    lwc_string *scheme = nsurl_get_component(url, NSURL_SCHEME);
    ctx->tls = (scheme != NULL &&
                strcmp(lwc_string_data(scheme), "https") == 0);
    if (scheme) lwc_string_unref(scheme);

    ctx->host = dup_component(url, NSURL_HOST);

    char *p = dup_component(url, NSURL_PATH);
    char *q = dup_component(url, NSURL_QUERY);
    if (p == NULL) p = strdup("/");
    if (q != NULL && *q != 0) {
        /* nsurl STRIPS the '?' when it stores the query -- see the URL_QUERY
         * case in utils/nsurl/parse.c, which starts the component one byte
         * past it. So it has to be put back. Without it a search for "x"
         * asks the server for /searchq=x, and the server answers 404, which
         * is the correct answer to the question actually asked. */
        size_t n = strlen(p) + 1 + strlen(q) + 1;
        char *both = malloc(n);
        if (both != NULL) { snprintf(both, n, "%s?%s", p, q); free(p); p = both; }
    }
    free(q);
    ctx->path = p;

    char *port = dup_component(url, NSURL_PORT);
    ctx->port = (port != NULL) ? atoi(port) : (ctx->tls ? 443 : 80);
    free(port);
    if (ctx->port <= 0) ctx->port = ctx->tls ? 443 : 80;

    if (post_urlenc != NULL) {
        ctx->bodylen = strlen(post_urlenc);
        ctx->body = malloc(ctx->bodylen + 1);
        if (ctx->body) memcpy(ctx->body, post_urlenc, ctx->bodylen + 1);
    }

    /* Whatever NetSurf wants added, minus anything the kernel already
     * writes: two User-Agent lines in one request is a worse answer than
     * one. */
    if (headers != NULL) {
        size_t need = 1;
        for (int i = 0; headers[i]; i++)
            if (!header_is_kernels(headers[i]))
                need += strlen(headers[i]) + 2;
        ctx->xhdr = malloc(need);
        if (ctx->xhdr != NULL) {
            ctx->xhdr[0] = 0;
            char *at = ctx->xhdr;
            for (int i = 0; headers[i]; i++) {
                if (header_is_kernels(headers[i])) continue;
                at += sprintf(at, "%s\r\n", headers[i]);
            }
        }
    }

    if (ctx->host == NULL || ctx->path == NULL) {
        nsurl_unref(ctx->url);
        free(ctx->host); free(ctx->path); free(ctx->body); free(ctx->xhdr);
        free(ctx);
        return NULL;
    }

    RING_INSERT(ring, ctx);
    return ctx;
}

static bool fetch_xyuos_start(void *ctx) {
    (void)ctx;
    /* Nothing to do: the poll below picks the queue up in order, because
     * only one request can be in the kernel at a time. */
    return true;
}

static void fetch_xyuos_abort(void *ctx) {
    struct fetch_xyuos_context *c = ctx;
    /* Flagged rather than torn down, so the poll loop never finds the
     * context it is working on gone. If it is the one in the kernel, the
     * kernel finishes it and the result is thrown away -- there is no way to
     * call off a fetch that has already been asked for. */
    c->aborted = true;
}

static void fetch_xyuos_free(void *ctx) {
    struct fetch_xyuos_context *c = ctx;
    if (inflight == c) inflight = NULL;
    nsurl_unref(c->url);
    free(c->host);
    free(c->path);
    free(c->body);
    free(c->xhdr);
    RING_REMOVE(ring, c);
    free(c);
}

/* --- reading what came back ---------------------------------------------- */

/* Hands each header line to NetSurf, and picks out of them the two things
 * this code needs to act on itself. */
static int header_is(const char *line, const char *name) {
    return strncasecmp(line, name, strlen(name)) == 0;
}

/* Hands each header line to NetSurf, and picks out of them the two things
 * this code needs to act on itself.
 *
 * Read twice on purpose. Whether Content-Length still describes anything
 * depends on headers that may come after it -- a server is free to send it
 * before Content-Encoding -- so the flags are all gathered before a single
 * line is passed on. */
static void deliver_headers(struct fetch_xyuos_context *c,
                            char *head, int headlen,
                            char *location, int locmax, int *gzipped,
                            int *chunked) {
    location[0] = 0;
    *gzipped = 0;
    *chunked = 0;

    /* Past the status line: NetSurf is told the code separately. */
    int first = 0;
    while (first < headlen && head[first] != '\n') first++;
    first++;

    for (int pass = 0; pass < 2; pass++) {
        int at = first;
        while (at < headlen) {
            int end = at;
            while (end < headlen && head[end] != '\n') end++;
            int stop = end;
            while (stop > at && (head[stop - 1] == '\r' || head[stop - 1] == ' '))
                stop--;
            if (stop <= at) { at = end + 1; continue; }

            char save = head[stop];
            head[stop] = 0;
            char *line = head + at;

            /* Anything that would describe the body as it arrived rather
             * than as NetSurf is about to receive it. */
            int drop = header_is(line, "content-encoding:")
                    || header_is(line, "transfer-encoding:")
                    || header_is(line, "x-xyuos-dechunked:")
                    || ((*gzipped || *chunked) &&
                        header_is(line, "content-length:"));

            if (pass == 0) {
                if (header_is(line, "location:")) {
                    const char *v = line + 9;
                    while (*v == ' ') v++;
                    snprintf(location, (size_t)locmax, "%s", v);
                }
                /* The body is handed on in the clear, so this would be a lie
                 * by the time NetSurf saw it. */
                if (header_is(line, "content-encoding:")) {
                    const char *v = line + 17;
                    while (*v == ' ') v++;
                    if (*v == 'g' || *v == 'x' || *v == 'd') *gzipped = 1;
                }
                /* The kernel put the pieces back together and renamed the
                 * header to say so. Either name means the length that came
                 * with it counted framing, not content. */
                if (header_is(line, "transfer-encoding:") ||
                    header_is(line, "x-xyuos-dechunked:")) {
                    const char *v = line + 18;
                    while (*v == ' ') v++;
                    if (strncasecmp(v, "chunked", 7) == 0) *chunked = 1;
                }
            } else if (!drop && !c->aborted) {
                fetch_msg msg;
                msg.type = FETCH_HEADER;
                msg.data.header_or_data.buf = (const uint8_t *)line;
                msg.data.header_or_data.len = (size_t)(stop - at);
                send_msg(&msg, c);
            }

            head[stop] = save;
            at = end + 1;
        }
    }
}

static void finish(struct fetch_xyuos_context *c, int n) {
    fetch_msg msg;

    if (n <= 0) {
        send_error(c, "the fetch did not complete");
        return;
    }

    /* Where the headers stop. */
    int body = -1;
    for (int i = 0; i + 3 < n; i++) {
        if (rxbuf[i] == '\r' && rxbuf[i + 1] == '\n' &&
            rxbuf[i + 2] == '\r' && rxbuf[i + 3] == '\n') { body = i + 4; break; }
        if (rxbuf[i] == '\n' && rxbuf[i + 1] == '\n') { body = i + 2; break; }
    }
    if (body < 0) { send_error(c, "the server's answer had no headers"); return; }

    /* HTTP/1.x CODE */
    int code = 0;
    {
        int i = 0;
        while (i < body && rxbuf[i] != ' ') i++;
        while (i < body && rxbuf[i] == ' ') i++;
        while (i < body && rxbuf[i] >= '0' && rxbuf[i] <= '9')
            code = code * 10 + (rxbuf[i++] - '0');
    }
    if (code == 0) { send_error(c, "the server's answer had no status"); return; }
    fetch_set_http_code(c->parent_fetch, code);

    char location[1024];
    int gzipped = 0, chunked = 0;
    deliver_headers(c, rxbuf, body, location, (int)sizeof location,
                    &gzipped, &chunked);
    if (c->aborted) return;

    /* A redirect replaces everything that would have followed. */
    if (code >= 300 && code <= 399 && location[0] != 0) {
        msg.type = FETCH_REDIRECT;
        msg.data.redirect = location;      /* may be relative; llcache joins */
        send_msg(&msg, c);
        return;
    }

    if (c->only_2xx && (code < 200 || code >= 300)) {
        send_error(c, "the server did not answer with success");
        return;
    }

    const unsigned char *data = (const unsigned char *)rxbuf + body;
    int len = n - body;
    unsigned char *freeme = NULL;

    if (gzipped && len > 0) {
        int at = gzip_body_at(data, len);
        if (at < 0) at = zlib_body_at(data, len);
        unsigned long got = 0;
        unsigned char *out = inflate_raw(data + at,
                                         (unsigned long)(len - at), &got);
        if (out == NULL || got == 0) {
            free(out);
            send_error(c, "the server compressed that in a way we could not read");
            return;
        }
        freeme = out;
        data = out;
        len = (int)got;
    }

    if (!c->aborted && len > 0) {
        msg.type = FETCH_DATA;
        msg.data.header_or_data.buf = data;
        msg.data.header_or_data.len = (size_t)len;
        send_msg(&msg, c);
    }
    if (!c->aborted) {
        msg.type = FETCH_FINISHED;
        send_msg(&msg, c);
    }
    free(freeme);
}

/* --- the poll ------------------------------------------------------------ */

/* Whoever is in the kernel is done with: report it and let it go. */
static void retire(struct fetch_xyuos_context *c) {
    inflight = NULL;
    fetch_remove_from_queues(c->parent_fetch);
    fetch_free(c->parent_fetch);
}

static void fetch_xyuos_poll(lwc_string *scheme) {
    (void)scheme;

    if (inflight != NULL) {
        int progress = 0;
        int r;

        /* Each check hands the kernel's fetch task one four-millisecond
         * slice and comes back. NetSurf asks fetchers to poll every ten
         * milliseconds, so one slice per call would leave the network idle
         * most of the time and a page would take as long to arrive as the
         * polling allowed rather than as long as the network needed.
         *
         * So take several slices while the fetch is still running, bounded
         * by a frame's worth of time: long enough to keep the connection
         * busy, short enough that a key press is still answered promptly. */
        unsigned deadline = uptime_ms() + 16;
        do {
            r = net_fetch_check(&progress);
        } while (r == NET_FETCH_PENDING && uptime_ms() < deadline);

        if (r == NET_FETCH_PENDING) {
            if (!inflight->aborted && progress > inflight->reported + 4096) {
                char note[64];
                snprintf(note, sizeof note, "%d bytes", progress);
                fetch_msg msg;
                msg.type = FETCH_PROGRESS;
                msg.data.progress = note;
                send_msg(&msg, inflight);
                inflight->reported = progress;
            }
            return;
        }

        int n = (r >= 0) ? net_fetch_done(rxbuf, XY_MAX) : -1;
        struct fetch_xyuos_context *c = inflight;
        if (c->aborted) {
            /* Asked for, arrived, no longer wanted. */
            retire(c);
        } else {
            finish(c, n);
            retire(c);
        }
        /* One kernel slot: whoever is next waits for the next poll. */
        return;
    }

    /* Nothing running. Take the first in the queue that still wants to go. */
    if (ring == NULL) return;

    struct fetch_xyuos_context *c = ring, *next;
    do {
        next = c->r_next;
        if (c->aborted && !c->sent) {
            retire(c);
        } else if (!c->sent) {
            int ok = net_fetch_begin(c->host, c->path, c->port, c->tls,
                                     1,            /* headers as well */
                                     c->body, (int)c->bodylen,
                                     c->xhdr);
            if (!ok) return;                       /* busy: try again later */
            c->sent = true;
            inflight = c;
            return;
        }
        c = next;
    } while (c != ring && ring != NULL);
}

nserror fetch_xyuos_register(void) {
    const struct fetcher_operation_table ops = {
        .initialise = fetch_xyuos_initialise,
        .acceptable = fetch_xyuos_can_fetch,
        .setup      = fetch_xyuos_setup,
        .start      = fetch_xyuos_start,
        .abort      = fetch_xyuos_abort,
        .free       = fetch_xyuos_free,
        .poll       = fetch_xyuos_poll,
        /* No fdset: there is no descriptor to wait on, so NetSurf keeps
         * polling on its own timer, which is what this needs. */
        .finalise   = fetch_xyuos_finalise
    };

    nserror e = fetcher_add(lwc_string_ref(corestring_lwc_http), &ops);
    if (e != NSERROR_OK) return e;
    return fetcher_add(lwc_string_ref(corestring_lwc_https), &ops);
}
