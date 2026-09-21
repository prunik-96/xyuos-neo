// Additional <string.h> / <stdlib.h> routines a C compiler leans on.
// Kept in their own translation unit so the original string.c/stdlib.c stay
// as they were.

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>

int errno = 0;

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

int strcasecmp(const char *a, const char *b) {
    while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = lc((unsigned char)a[i]), cb = lc((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) break;
    }
    return (char *)last;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return NULL;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    while (i < n && src[i]) { d[i] = src[i]; i++; }
    d[i] = '\0';
    return dst;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    for (; s[n]; n++) if (!strchr(accept, s[n])) break;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; s[n]; n++) if (strchr(reject, s[n])) break;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++)
        if (strchr(accept, *s)) return (char *)s;
    return NULL;
}

// Only the codes this libc actually produces; anything else is reported
// honestly as unknown rather than given a made-up description.
char *strerror(int e) {
    switch (e) {
        case 0:       return "no error";
        case ENOENT:  return "no such file or directory";
        case EIO:     return "I/O error";
        case EBADF:   return "bad file descriptor";
        case ENOMEM:  return "out of memory";
        case EACCES:  return "permission denied";
        case EEXIST:  return "file exists";
        case ENOTDIR: return "not a directory";
        case EISDIR:  return "is a directory";
        case EINVAL:  return "invalid argument";
        case ENOSPC:  return "no space left on device";
        case ESPIPE:  return "illegal seek";
        case ERANGE:  return "result out of range";
        default:      return "unknown error";
    }
}

// --- number parsing -------------------------------------------------------
long strtol(const char *s, char **end, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (*p == '0') ? 8 : 10;
    }

    long value = 0;
    int any = 0;
    for (;; p++) {
        int d;
        if (isdigit((unsigned char)*p))      d = *p - '0';
        else if (isalpha((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
        else break;
        if (d >= base) break;
        value = value * base + d;
        any = 1;
    }

    if (end) *end = (char *)(any ? p : s);
    return neg ? -value : value;
}

unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtol(s, end, base);
}

// long and long long are both 64-bit here, so these are the same parse.
unsigned long long strtoull(const char *s, char **end, int base) {
    return (unsigned long long)strtol(s, end, base);
}

long long strtoll(const char *s, char **end, int base) {
    return (long long)strtol(s, end, base);
}

long atol(const char *s) { return strtol(s, NULL, 10); }

int abs(int v)   { return v < 0 ? -v : v; }
long labs(long v){ return v < 0 ? -v : v; }

// --- qsort ----------------------------------------------------------------
// Insertion sort over the raw bytes. O(n^2), but the arrays a compiler sorts
// (symbol lists and the like) are small, and this is far easier to trust than
// a hand-rolled quicksort with a byte-wise swap.
/* Halve the range until the answer is in hand or the range is empty. The
 * array has to be sorted by the same comparison, which is the caller's half
 * of the bargain. */
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*cmp)(const void *, const void *)) {
    const char *p = (const char *)base;
    while (count > 0) {
        size_t half = count / 2;
        const char *mid = p + half * size;
        int r = cmp(key, mid);
        if (r == 0) return (void *)mid;
        if (r > 0) {
            p = mid + size;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    return 0;
}

void qsort(void *base, size_t count, size_t size,
           int (*cmp)(const void *, const void *)) {
    char *a = (char *)base;
    for (size_t i = 1; i < count; i++) {
        for (size_t j = i; j > 0; j--) {
            char *cur = a + j * size;
            char *prev = cur - size;
            if (cmp(prev, cur) <= 0) break;
            for (size_t k = 0; k < size; k++) {
                char t = prev[k];
                prev[k] = cur[k];
                cur[k] = t;
            }
        }
    }
}

// No environment on this OS; tcc calls getenv() during setup and must simply
// get "unset" back.
char *getenv(const char *name) { (void)name; return NULL; }
