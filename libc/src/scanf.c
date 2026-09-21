/* scanf family.
 *
 * One engine over an abstract source, so the string and stream forms cannot
 * drift apart -- the same arrangement the printf side uses.
 *
 * Terminal input is gathered line-at-a-time by the kernel (see SYS_READSTD),
 * so a blocking scanf() reads a whole line the user has already been able to
 * see and correct. That is why nothing here has to echo or handle backspace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

struct src {
    const char *str;    /* non-NULL for sscanf */
    int         pos;
    FILE       *f;      /* non-NULL for fscanf/scanf */
    int         back;   /* one-character pushback, or -2 when empty */
    int         nread;  /* characters consumed, for %n */
};

static int sc_get(struct src *s) {
    if (s->back != -2) { int c = s->back; s->back = -2; s->nread++; return c; }
    int c;
    if (s->str) c = s->str[s->pos] ? (unsigned char)s->str[s->pos++] : EOF;
    else        c = fgetc(s->f);
    if (c != EOF) s->nread++;
    return c;
}

static void sc_unget(struct src *s, int c) {
    if (c == EOF) return;
    s->back = c;
    s->nread--;
}

static int sc_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static void skip_space(struct src *s) {
    int c;
    do { c = sc_get(s); } while (c != EOF && sc_space(c));
    sc_unget(s, c);
}

/* The standard distinguishes two failures and callers depend on it:
 *   INPUT failure    -- ran out of input before any conversion  -> EOF
 *   MATCHING failure -- input is there but does not fit         -> count so far
 * Conflating them makes `if (scanf("%d", &n) != 1)` unable to tell a typed
 * word from a closed input. */
static int at_eof(struct src *s) {
    int c = sc_get(s);
    if (c == EOF) return 1;
    sc_unget(s, c);
    return 0;
}

/* Collect a numeric token into buf, then let strtol/strtod interpret it --
 * duplicating the parsing here would be a second place to get bases and signs
 * wrong. */
#define TOK_MAX 128

static int collect_int(struct src *s, char *buf, int width, int base, int is_signed) {
    int n = 0;
    int c = sc_get(s);

    if (is_signed && (c == '+' || c == '-') && n < width) { buf[n++] = (char)c; c = sc_get(s); }

    if (base == 16 || base == 0) {
        if (c == '0' && n + 1 < width) {
            buf[n++] = (char)c;
            c = sc_get(s);
            if ((c == 'x' || c == 'X') && n < width) { buf[n++] = (char)c; c = sc_get(s); }
        }
    }
    while (c != EOF && n < width && n < TOK_MAX - 1) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        int b = (base == 0) ? 16 : base;   /* be generous; strtol re-checks */
        if (d >= b) break;
        buf[n++] = (char)c;
        c = sc_get(s);
    }
    sc_unget(s, c);
    buf[n] = '\0';
    return n;
}

static int collect_float(struct src *s, char *buf, int width) {
    int n = 0;
    int c = sc_get(s);

    if ((c == '+' || c == '-') && n < width) { buf[n++] = (char)c; c = sc_get(s); }
    while (c != EOF && n < width && n < TOK_MAX - 1 &&
           ((c >= '0' && c <= '9') || c == '.')) {
        buf[n++] = (char)c;
        c = sc_get(s);
    }
    if ((c == 'e' || c == 'E') && n < width && n < TOK_MAX - 2) {
        buf[n++] = (char)c;
        c = sc_get(s);
        if ((c == '+' || c == '-') && n < width) { buf[n++] = (char)c; c = sc_get(s); }
        while (c != EOF && n < width && n < TOK_MAX - 1 && c >= '0' && c <= '9') {
            buf[n++] = (char)c;
            c = sc_get(s);
        }
    }
    sc_unget(s, c);
    buf[n] = '\0';
    return n;
}

static int vscan(struct src *s, const char *fmt, va_list ap) {
    int assigned = 0;
    char tok[TOK_MAX];

    for (const char *f = fmt; *f; f++) {
        if (sc_space(*f)) {                       /* whitespace: skip any run */
            skip_space(s);
            continue;
        }
        if (*f != '%') {                          /* literal must match */
            int c = sc_get(s);
            if (c == EOF) return assigned ? assigned : EOF;
            if (c != (unsigned char)*f) { sc_unget(s, c); return assigned; }
            continue;
        }

        f++;
        if (*f == '%') {                          /* a literal percent */
            int c = sc_get(s);
            if (c != '%') { sc_unget(s, c); return assigned; }
            continue;
        }

        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }

        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (width == 0) width = TOK_MAX - 1;

        int lng = 0;                              /* 1 = l, 2 = ll/L */
        while (*f == 'h') f++;                    /* short: value still fits */
        while (*f == 'l' || *f == 'L') { lng += (*f == 'L') ? 2 : 1; f++; }
        if (lng > 2) lng = 2;

        switch (*f) {
        case 'c': {
            int want = (width == TOK_MAX - 1) ? 1 : width;   /* %c has no skip */
            char *out = suppress ? NULL : va_arg(ap, char *);
            for (int i = 0; i < want; i++) {
                int c = sc_get(s);
                if (c == EOF) return (i || assigned) ? assigned : EOF;
                if (out) out[i] = (char)c;
            }
            if (!suppress) assigned++;
            break;
        }
        case 's': {
            skip_space(s);
            if (at_eof(s)) return assigned ? assigned : EOF;
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0, c;
            while ((c = sc_get(s)) != EOF && !sc_space(c) && n < width) {
                if (out) out[n] = (char)c;
                n++;
            }
            sc_unget(s, c);
            if (n == 0) return assigned;                  /* matching failure */
            if (out) out[n] = '\0';
            if (!suppress) assigned++;
            break;
        }
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            int base = (*f == 'o') ? 8 : (*f == 'x' || *f == 'X') ? 16
                     : (*f == 'i') ? 0 : 10;
            int is_signed = (*f == 'd' || *f == 'i');
            skip_space(s);
            if (at_eof(s)) return assigned ? assigned : EOF;
            if (collect_int(s, tok, width, base, is_signed) == 0)
                return assigned;                          /* matching failure */
            long long v = (base == 16 || base == 0)
                        ? (long long)strtoll(tok, NULL, base ? base : 0)
                        : (long long)strtoll(tok, NULL, base);
            if (!suppress) {
                if (lng == 2)      *va_arg(ap, long long *) = v;
                else if (lng == 1) *va_arg(ap, long *) = (long)v;
                else               *va_arg(ap, int *) = (int)v;
                assigned++;
            }
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': {
            skip_space(s);
            if (at_eof(s)) return assigned ? assigned : EOF;
            if (collect_float(s, tok, width) == 0)
                return assigned;                          /* matching failure */
            double v = strtod(tok, NULL);
            if (!suppress) {
                if (lng >= 1) *va_arg(ap, double *) = v;
                else          *va_arg(ap, float *) = (float)v;
                assigned++;
            }
            break;
        }
        case 'n':                                  /* not an assignment */
            if (!suppress) *va_arg(ap, int *) = s->nread;
            break;
        default:
            return assigned;                       /* unknown conversion */
        }
    }
    return assigned;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    struct src s = { str, 0, NULL, -2, 0 };
    return vscan(&s, fmt, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    struct src s = { NULL, 0, f, -2, 0 };
    return vscan(&s, fmt, ap);
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}
