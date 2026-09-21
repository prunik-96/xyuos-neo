/* Exercise the zlib and iconv shims against known answers.
 *
 * Both are built here for the host, so that what they produce can be put
 * beside what the host's own zlib and iconv produce for the same input. A
 * shim that compiles proves nothing; this is the part that matters.
 *
 *   shim_probe gz     <file>              read it as gzopen/gzgets would
 *   shim_probe zs     <file>              read it through the z_stream calls
 *   shim_probe iconv  <from> <to> <file>  convert it
 *
 * Output goes to stdout as raw bytes, so the caller can compare it with
 * gunzip's or iconv's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <zlib.h>
#include <iconv.h>

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    unsigned char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

static int do_gz(const char *path) {
    gzFile g = gzopen(path, "r");
    if (!g) { fprintf(stderr, "gzopen failed: %s\n", strerror(errno)); return 2; }
    char line[4096];
    while (gzgets(g, line, sizeof line))
        fwrite(line, 1, strlen(line), stdout);
    gzclose(g);
    return 0;
}

/* gzgets hands back a C string, so bytes cannot come through it past a NUL.
 * gzread is the way to read arbitrary bytes, and this is here so the tests
 * can check binary content without blaming gzgets for the obvious. */
static int do_gzread(const char *path) {
    gzFile g = gzopen(path, "r");
    if (!g) { fprintf(stderr, "gzopen failed: %s\n", strerror(errno)); return 2; }
    unsigned char buf[97];              /* an awkward size, on purpose */
    int n;
    while ((n = gzread(g, buf, sizeof buf)) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
    gzclose(g);
    return n < 0 ? 2 : 0;
}

static int do_zstream(const char *path) {
    size_t n;
    unsigned char *data = slurp(path, &n);

    z_stream s;
    s.zalloc = Z_NULL; s.zfree = Z_NULL; s.opaque = Z_NULL;
    s.next_in = data;
    s.avail_in = (unsigned)n;

    int rc = inflateInit2(&s, 32 + MAX_WBITS);
    if (rc != Z_OK) { fprintf(stderr, "inflateInit2: %d\n", rc); return 2; }

    /* Deliberately small, so the output has to come back over many calls --
     * that is the pattern the caller in NetSurf uses. */
    unsigned char out[64];
    do {
        s.next_out = out;
        s.avail_out = sizeof out;
        rc = inflate(&s, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            fprintf(stderr, "inflate: %d (%s)\n", rc, s.msg ? s.msg : "");
            inflateEnd(&s);
            return 2;
        }
        fwrite(out, 1, sizeof out - s.avail_out, stdout);
    } while (rc != Z_STREAM_END);

    inflateEnd(&s);
    free(data);
    return 0;
}

static int do_iconv(const char *from, const char *to, const char *path) {
    size_t n;
    unsigned char *data = slurp(path, &n);

    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        fprintf(stderr, "iconv_open(%s, %s): %s\n", to, from, strerror(errno));
        return 2;
    }

    char *in = (char *)data;
    size_t inleft = n;
    /* Small on purpose: the output filling up mid-way is the case where an
     * iconv has to leave the input pointer somewhere exactly right. */
    char buf[16];
    while (inleft > 0) {
        char *out = buf;
        size_t outleft = sizeof buf;
        size_t r = iconv(cd, &in, &inleft, &out, &outleft);
        fwrite(buf, 1, sizeof buf - outleft, stdout);
        if (r == (size_t)-1) {
            if (errno == E2BIG) continue;          /* room made, go again */
            fprintf(stderr, "iconv: %s at offset %ld\n",
                    strerror(errno), (long)(in - (char *)data));
            iconv_close(cd);
            return 3;
        }
    }
    iconv_close(cd);
    free(data);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "gz") == 0)    return do_gz(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "zr") == 0)    return do_gzread(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "zs") == 0)    return do_zstream(argv[2]);
    if (argc >= 5 && strcmp(argv[1], "iconv") == 0)
        return do_iconv(argv[2], argv[3], argv[4]);
    fprintf(stderr, "usage: shim_probe gz|zs <file> | iconv <from> <to> <file>\n");
    return 2;
}
