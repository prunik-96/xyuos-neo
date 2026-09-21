#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Streams are unbuffered. Formatting happens once, in format(), against a
// "sink" that is either a stream or a caller-supplied buffer -- that is what
// lets printf/fprintf/snprintf share one implementation instead of three.
//
// Descriptors are the POSIX ones from libc/src/posix.c: 0/1/2 are the standard
// streams and real files start at 3.

// --- the three standard streams -------------------------------------------
static FILE std_in  = { 0, 0, 0, -1 };
static FILE std_out = { 1, 0, 0, -1 };
static FILE std_err = { 2, 0, 0, -1 };

FILE *stdin  = &std_in;
FILE *stdout = &std_out;
FILE *stderr = &std_err;

// --- raw stream access ----------------------------------------------------
static long stream_write(FILE *f, const void *buf, size_t len) {
    if (!f || len == 0) return 0;
    long n = write((int)f->fd, buf, len);
    if (n < 0) { f->err = 1; return -1; }
    return n;
}

static long stream_read(FILE *f, void *buf, size_t len) {
    if (!f || len == 0) return 0;
    long n = read((int)f->fd, buf, len);
    if (n < 0) { f->err = 1; return -1; }
    if (n == 0) f->eof = 1;
    return n;
}

// --- output sink ----------------------------------------------------------
struct sink {
    char  *buf;     // non-NULL: write into this buffer (snprintf family)
    size_t cap;     // buffer capacity including the NUL
    FILE  *f;       // non-NULL: write to this stream
    size_t len;     // chars that WOULD have been written (snprintf semantics)
};

static void sink_write(struct sink *s, const char *p, size_t n) {
    if (n == 0) return;
    if (s->buf) {
        for (size_t i = 0; i < n; i++) {
            if (s->len + i + 1 < s->cap) s->buf[s->len + i] = p[i];
        }
        s->len += n;
        return;
    }
    stream_write(s->f, p, n);
    s->len += n;
}

static void sink_char(struct sink *s, char c) { sink_write(s, &c, 1); }

static void sink_pad(struct sink *s, char c, int n) {
    for (int i = 0; i < n; i++) sink_char(s, c);
}

// --- number formatting ----------------------------------------------------
static int utoa_buf(uint64_t v, unsigned base, int upper, char *out) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = digits[v % base]; v /= base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

// --- floating point -------------------------------------------------------
//
// Fixed-point conversion by repeated multiplication, not a shortest-repr
// algorithm: the digits are correct to the precision asked for, which is what
// %f promises, but this is not Grisu and will not round-trip a double through
// %.17g. Good enough for printing results; not for serialising them.
//
// The magnitude limit is deliberate. Converting 1e300 in fixed notation needs
// 300 integer digits, so anything that large is rendered in %e form instead of
// silently overflowing the buffer.

#define FLOAT_BUF 512
#define FLOAT_MAX_FIXED 1e18

static int is_nan(double d)  { return d != d; }
static int is_inf(double d)  { return !is_nan(d) && (d > 1.7976931348623157e308 ||
                                                     d < -1.7976931348623157e308); }

// Round `frac` (0 <= frac < 1) to `prec` digits, writing them into out.
// Returns the number of digits written and sets *carry if rounding overflowed
// back into the integer part.
static int frac_digits(double frac, int prec, char *out, int *carry) {
    *carry = 0;
    for (int i = 0; i < prec; i++) {
        frac *= 10.0;
        int d = (int)frac;
        if (d > 9) d = 9;
        out[i] = (char)('0' + d);
        frac -= d;
    }
    if (frac >= 0.5) {                        // round half up
        int i = prec - 1;
        for (; i >= 0; i--) {
            if (out[i] != '9') { out[i]++; break; }
            out[i] = '0';
        }
        if (i < 0) *carry = 1;
    }
    return prec;
}

static int format_double(double d, char conv, int prec, int plus, int space,
                         char *out) {
    int n = 0;
    char sign = 0;

    if (d < 0 || (d == 0 && 1.0 / d < 0)) { sign = '-'; d = -d; }
    else if (plus) sign = '+';
    else if (space) sign = ' ';

    if (is_nan(d)) {
        if (sign) out[n++] = sign;
        out[n++] = 'n'; out[n++] = 'a'; out[n++] = 'n';
        return n;
    }
    if (is_inf(d)) {
        if (sign) out[n++] = sign;
        out[n++] = 'i'; out[n++] = 'n'; out[n++] = 'f';
        return n;
    }

    if (prec < 0) prec = 6;
    if (prec > 100) prec = 100;               // keeps the buffer bounded

    int exp10 = 0;
    int sci = (conv == 'e' || conv == 'E');

    // %g picks the shorter of %e and %f, as C requires.
    if (conv == 'g' || conv == 'G') {
        if (prec == 0) prec = 1;
        double t = d;
        if (t != 0) { while (t >= 10.0) { t /= 10.0; exp10++; }
                      while (t < 1.0)   { t *= 10.0; exp10--; } }
        sci = (exp10 < -4 || exp10 >= prec);
        prec = sci ? prec - 1 : prec - 1 - exp10;
        if (prec < 0) prec = 0;
        exp10 = 0;
    }

    if (sci) {
        if (d != 0) {
            while (d >= 10.0) { d /= 10.0; exp10++; }
            while (d < 1.0)   { d *= 10.0; exp10--; }
        }
    } else if (d >= FLOAT_MAX_FIXED) {
        // Too big for fixed notation; fall back to scientific rather than
        // overrun the buffer.
        sci = 1;
        while (d >= 10.0) { d /= 10.0; exp10++; }
    }

    uint64_t ip = (uint64_t)d;
    double frac = d - (double)ip;

    char fd[128];
    int carry = 0;
    int flen = frac_digits(frac, prec, fd, &carry);
    if (carry) {
        ip++;
        if (sci && ip >= 10) { ip /= 10; exp10++; }
    }

    if (sign) out[n++] = sign;
    n += utoa_buf(ip, 10, 0, out + n);
    if (flen > 0) {
        out[n++] = '.';
        for (int i = 0; i < flen; i++) out[n++] = fd[i];
    }

    if (sci) {
        out[n++] = (conv == 'E' || conv == 'G') ? 'E' : 'e';
        out[n++] = (exp10 < 0) ? '-' : '+';
        int e = exp10 < 0 ? -exp10 : exp10;
        if (e < 10) out[n++] = '0';           // exponent is at least two digits
        n += utoa_buf((uint64_t)e, 10, 0, out + n);
    }
    return n;
}

// --- the format engine ----------------------------------------------------
// Supports %[-0+ ][width][.prec][l|ll|z]{d,i,u,x,X,p,c,s,f,e,g,%}.
static int format(struct sink *s, const char *fmt, va_list args) {
    while (*fmt) {
        if (*fmt != '%') {
            const char *start = fmt;              // emit literal runs in one go
            while (*fmt && *fmt != '%') fmt++;
            sink_write(s, start, (size_t)(fmt - start));
            continue;
        }
        fmt++;
        if (*fmt == '%') { sink_char(s, '%'); fmt++; continue; }

        int left = 0, zero = 0, plus = 0, space = 0;
        for (;;) {
            if (*fmt == '-')      { left = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { space = 1; fmt++; }
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(args, int); fmt++;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(args, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 2; fmt++; }
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }

        char conv = *fmt ? *fmt++ : 0;
        char num[40];
        int  nlen = 0;
        char sign = 0;

        switch (conv) {
            case 'd': case 'i': {
                int64_t v = (lng >= 1) ? (int64_t)va_arg(args, long) : (int64_t)va_arg(args, int);
                uint64_t mag;
                if (v < 0) { sign = '-'; mag = (uint64_t)(-(v + 1)) + 1ULL; }
                else       { if (plus) sign = '+'; mag = (uint64_t)v; }
                nlen = utoa_buf(mag, 10, 0, num);
                break;
            }
            case 'u': {
                uint64_t v = (lng >= 1) ? (uint64_t)va_arg(args, unsigned long)
                                        : (uint64_t)va_arg(args, unsigned int);
                nlen = utoa_buf(v, 10, 0, num);
                break;
            }
            case 'x': case 'X': {
                uint64_t v = (lng >= 1) ? (uint64_t)va_arg(args, unsigned long)
                                        : (uint64_t)va_arg(args, unsigned int);
                nlen = utoa_buf(v, 16, conv == 'X', num);
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)(uintptr_t)va_arg(args, void *);
                num[0] = '0'; num[1] = 'x';
                nlen = 2 + utoa_buf(v, 16, 0, num + 2);
                break;
            }
            case 'c': {
                num[0] = (char)va_arg(args, int);
                nlen = 1;
                break;
            }
            case 's': {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";
                int slen = 0;
                while (str[slen] && (prec < 0 || slen < prec)) slen++;
                if (!left) sink_pad(s, ' ', width - slen);
                sink_write(s, str, (size_t)slen);
                if (left) sink_pad(s, ' ', width - slen);
                continue;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                double d = va_arg(args, double);
                char fbuf[FLOAT_BUF];
                int flen = format_double(d, conv, prec, plus, space, fbuf);
                if (!left) sink_pad(s, zero ? '0' : ' ', width - flen);
                sink_write(s, fbuf, (size_t)flen);
                if (left) sink_pad(s, ' ', width - flen);
                continue;
            }
            default:
                sink_char(s, '%');
                if (conv) sink_char(s, conv);
                continue;
        }

        // numeric padding: zeros go after the sign, spaces before it
        int zeros = (prec > nlen) ? prec - nlen : 0;
        int total = nlen + zeros + (sign ? 1 : 0);
        if (!left && zero && prec < 0 && width > total) {
            zeros += width - total;
            total = width;
        }
        if (!left) sink_pad(s, ' ', width - total);
        if (sign) sink_char(s, sign);
        sink_pad(s, '0', zeros);
        sink_write(s, num, (size_t)nlen);
        if (left) sink_pad(s, ' ', width - total);
    }
    return (int)s->len;
}

// --- printf family --------------------------------------------------------
int vfprintf(FILE *f, const char *fmt, va_list args) {
    struct sink s = { 0, 0, f, 0 };
    return format(&s, fmt, args);
}

int vprintf(const char *fmt, va_list args) { return vfprintf(stdout, fmt, args); }

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    struct sink s = { buf, size, 0, 0 };
    int n = format(&s, fmt, args);
    if (buf && size > 0) buf[(s.len < size) ? s.len : size - 1] = '\0';
    return n;
}

int printf(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    int r = vprintf(fmt, a);
    va_end(a);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    int r = vfprintf(f, fmt, a);
    va_end(a);
    return r;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    int r = vsnprintf(buf, size, fmt, a);
    va_end(a);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, a);
    va_end(a);
    return r;
}

// --- character I/O --------------------------------------------------------
int putchar(int c) {
    char ch = (char)c;
    stream_write(stdout, &ch, 1);
    return c;
}

int puts(const char *s) {
    stream_write(stdout, s, strlen(s));
    putchar('\n');
    return 0;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    if (stream_write(f, &ch, 1) != 1) return EOF;
    return c;
}

int putc(int c, FILE *f) { return fputc(c, f); }

int fgetc(FILE *f) {
    if (!f) return EOF;
    if (f->ungot != -1) { int c = f->ungot; f->ungot = -1; return c; }
    unsigned char ch;
    if (stream_read(f, &ch, 1) <= 0) { f->eof = 1; return EOF; }
    return (int)ch;
}

int getc(FILE *f) { return fgetc(f); }

int ungetc(int c, FILE *f) {
    if (!f || c == EOF || f->ungot != -1) return EOF;
    f->ungot = c;
    f->eof = 0;
    return c;
}

int getchar(void) { return fgetc(stdin); }

char *fgets(char *buf, int size, FILE *f) {
    if (!buf || size <= 0 || !f) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return (stream_write(f, s, n) == (long)n) ? (int)n : EOF;
}

// --- streams --------------------------------------------------------------
FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return NULL;

    // open() handles creation, truncation and append positioning, so the
    // modes map straight onto flags.
    int flags;
    switch (mode[0]) {
        case 'r': flags = O_RDONLY; break;
        case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case 'a': flags = O_WRONLY | O_CREAT | O_APPEND; break;
        default:  return NULL;
    }

    int fd = open(path, flags);
    if (fd < 0) return NULL;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    f->eof = 0;
    f->err = 0;
    f->ungot = -1;
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    if (fd < 0) return NULL;
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) return NULL;
    f->fd = fd;
    f->eof = 0;
    f->err = 0;
    f->ungot = -1;
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
    if (!f) return NULL;
    if (f->fd >= 3) close((int)f->fd);          // leave std streams alone

    FILE *nf = fopen(path, mode);
    if (!nf) return NULL;

    f->fd = nf->fd;                              // rebind the caller's stream
    f->eof = 0;
    f->err = 0;
    f->ungot = -1;
    free(nf);
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    close((int)f->fd);
    if (f != stdin && f != stdout && f != stderr) free(f);
    return 0;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f) {
    if (!f || size == 0 || count == 0) return 0;
    size_t want = size * count;
    size_t got = 0;
    char *p = (char *)buf;

    if (f->ungot != -1) { p[got++] = (char)f->ungot; f->ungot = -1; }

    while (got < want) {
        long n = stream_read(f, p + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got / size;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f) {
    if (!f || size == 0 || count == 0) return 0;
    size_t want = size * count;
    long n = stream_write(f, buf, want);
    if (n <= 0) return 0;
    return (size_t)n / size;
}

int fseek(FILE *f, long offset, int whence) {
    if (!f) return -1;
    f->ungot = -1;                 // a pushed-back char does not survive a seek
    if (lseek((int)f->fd, offset, whence) < 0) { f->err = 1; return -1; }
    f->eof = 0;
    return 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    return lseek((int)f->fd, 0, SEEK_CUR);
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }

int fflush(FILE *f) { (void)f; return 0; }        // unbuffered: nothing to do

int feof(FILE *f)   { return f ? f->eof : 1; }
int ferror(FILE *f) { return f ? f->err : 1; }
void clearerr(FILE *f) { if (f) { f->eof = 0; f->err = 0; } }

int remove(const char *path) { return (int)xyuos_unlink(path); }
