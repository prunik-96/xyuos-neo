#ifndef UPATH_H
#define UPATH_H

/* Path handling shared by the userland programs that browse the filesystem.
 * Header-only so it needs no Makefile entry.
 *
 * The kernel deliberately does NOT resolve "." and ".." -- it takes absolute
 * paths only. Doing it here keeps that policy in userland, where a wrong
 * answer costs one process. */

#include <string.h>
#include <stdio.h>

#define UPATH_MAX 256

/* Resolve `in` against `cwd` into `out` (size UPATH_MAX), collapsing "." and
 * ".." and any run of slashes. `out` always ends up absolute. */
static __attribute__((unused)) void upath_resolve(const char *cwd, const char *in, char *out) {
    char tmp[UPATH_MAX * 2];
    if (in[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", in);
    } else if (strcmp(cwd, "/") == 0) {
        snprintf(tmp, sizeof(tmp), "/%s", in);
    } else {
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, in);
    }

    int stack[64], depth = 0, o = 1;
    out[0] = '/';
    for (int i = 0; tmp[i]; ) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        int start = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - start;

        if (len == 1 && tmp[start] == '.') continue;
        if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (depth > 0) o = stack[--depth];
            continue;
        }
        if (depth < 64 && o + len + 1 < UPATH_MAX) {
            stack[depth++] = o;
            if (o > 1) out[o++] = '/';
            for (int k = 0; k < len; k++) out[o++] = tmp[start + k];
        }
    }
    out[o > 1 ? o : 1] = '\0';
    if (o <= 1) { out[0] = '/'; out[1] = '\0'; }
}

/* Split a directory listing from xyuos_listdir(). Names are NEWLINE-separated
 * and directories carry a trailing '/'. Calls `fn(name, is_dir, ctx)` for each
 * entry, with the trailing slash already stripped. */
static __attribute__((unused)) void upath_each_entry(const char *buf, long n,
                             void (*fn)(const char *, int, void *), void *ctx) {
    char name[UPATH_MAX];
    for (long i = 0; i < n; ) {
        long j = i;
        while (j < n && buf[j] != '\n') j++;
        int len = (int)(j - i);
        if (len > 0) {
            int is_dir = (buf[j - 1] == '/');
            if (is_dir) len--;
            if (len > UPATH_MAX - 1) len = UPATH_MAX - 1;
            for (int k = 0; k < len; k++) name[k] = buf[i + k];
            name[len] = '\0';
            if (len > 0) fn(name, is_dir, ctx);
        }
        i = j + 1;
    }
}

#endif
