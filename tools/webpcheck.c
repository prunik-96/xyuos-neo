/* Decode an image with our own code, on the host, and write it out raw.
 *
 * tools/webpcheck.py compiles this with the host compiler and compares what
 * it produces against libwebp, byte for byte. WebP is an integer format:
 * there is exactly one correct answer for every pixel, so "looks about right"
 * is not a result. Anything that differs is a bug in our decoder.
 *
 * Output: "P6\n<w> <h>\n255\n" followed by the pixels, so the file is also
 * viewable without any tooling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../userland/img.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: webpcheck <in> <out.ppm>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    image_t im;
    if (!img_load(buf, (unsigned long)n, &im)) {
        fprintf(stderr, "decode failed: %s\n", img_err);
        return 1;
    }

    FILE *o = fopen(argv[2], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
    fprintf(o, "P6\n%d %d\n255\n", im.w, im.h);
    for (long i = 0; i < (long)im.w * im.h; i++) {
        unsigned v = im.px[i];
        fputc((int)((v >> 16) & 0xFF), o);
        fputc((int)((v >> 8) & 0xFF), o);
        fputc((int)(v & 0xFF), o);
    }
    fclose(o);
    fprintf(stderr, "%s: %dx%d\n", argv[1], im.w, im.h);
    return 0;
}
