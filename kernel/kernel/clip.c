// The clipboard: one piece of text, or one list of files, shared by every
// program. See clip.h.

#include "clip.h"
#include "../mm/heap.h"
#include <stddef.h>

static char *data;
static uint32_t len, cap;
static int type;
static uint32_t seq;

int clip_set(int t, const void *src, uint32_t n) {
    if (n > CLIP_MAX) return -1;
    if (n + 1 > cap) {
        char *b = (char *)kmalloc(n + 1);
        if (!b) return -1;
        if (data) kfree(data);
        data = b;
        cap = n + 1;
    }
    const char *s = (const char *)src;
    for (uint32_t i = 0; i < n; i++) data[i] = s[i];
    data[n] = 0;
    len = n;
    type = n ? t : CLIP_NONE;
    seq++;
    return 0;
}

uint32_t clip_get(void *dst, uint32_t max, int *t) {
    if (t) *t = type;
    uint32_t n = len < max ? len : max;
    char *d = (char *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = data[i];
    return len;
}

uint32_t clip_seq(void) { return seq; }
int clip_type(void) { return type; }
uint32_t clip_len(void) { return len; }
