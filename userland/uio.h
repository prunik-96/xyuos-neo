#ifndef UIO_H
#define UIO_H

/* Line-oriented input shared by the text tools.
 *
 * Every one of them reads stdin when given no file, which is what makes them
 * usable on the right-hand side of a pipe. Keeping that in one place means a
 * tool cannot accidentally support files but not pipes. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "upath.h"

#define ULINE_MAX 1024

struct ureader {
    int  fd;
    int  own;               /* close it when done */
    char buf[4096];
    int  len, pos;
};

static __attribute__((unused))
int ureader_open(struct ureader *r, const char *path) {
    r->len = r->pos = 0;
    if (!path) { r->fd = 0; r->own = 0; return 0; }

    char abs[UPATH_MAX];
    upath_resolve("/", path, abs);
    r->fd = open(abs, 0);
    r->own = 1;
    return (r->fd < 0) ? -1 : 0;
}

static __attribute__((unused))
void ureader_close(struct ureader *r) {
    if (r->own && r->fd >= 0) close(r->fd);
}

static __attribute__((unused))
int ureader_getc(struct ureader *r) {
    if (r->pos >= r->len) {
        long n = read(r->fd, r->buf, sizeof(r->buf));
        if (n <= 0) return -1;
        r->len = (int)n;
        r->pos = 0;
    }
    return (unsigned char)r->buf[r->pos++];
}

/* Read one line without its newline. Returns the length, or -1 at end of
 * input. A final line with no newline is still returned. */
static __attribute__((unused))
int ureader_line(struct ureader *r, char *out, int max) {
    int n = 0, c;
    while ((c = ureader_getc(r)) >= 0) {
        if (c == '\n') { out[n] = '\0'; return n; }
        if (n < max - 1) out[n++] = (char)c;
    }
    out[n] = '\0';
    return n > 0 ? n : -1;
}

#endif
