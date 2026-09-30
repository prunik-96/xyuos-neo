/* userland/svg.h on the host: an SVG file in, a picture out.
 *
 *   gcc -O2 -Wall -Iuserland -Ilibrast/include -Ilibrast/src \
 *       librast/src/[a-z]*.c tools/svgrender.c -o /tmp/svgrender
 *   /tmp/svgrender in.svg out.ppm [width height]
 *
 * The picture is laid on a checkerboard through its alpha, so what is
 * transparent can be seen to be. With a width and height it is drawn at
 * that size, the way NetSurf redraws a scaled drawing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: svgrender in.svg out.ppm [w h]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);

    img_background = 0x000000;          /* so px is the premultiplied colour */
    image_t im;
    int ok = argc >= 5 ? svg_render(d, (unsigned long)n, atoi(argv[3]), atoi(argv[4]), &im)
                       : img_load(d, (unsigned long)n, &im);
    if (!ok) { fprintf(stderr, "decode failed: %s\n", img_err); return 1; }
    int sw = 0, sh = 0;
    svg_size(d, (unsigned long)n, &sw, &sh);
    printf("%s: %dx%d (says %dx%d), %s\n", argv[1], im.w, im.h, sw, sh,
           im.alpha ? "with transparency" : "opaque");

    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "P6\n%d %d\n255\n", im.w, im.h);
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            unsigned v = im.px[y * im.w + x], a = im.alpha ? im.alpha[y * im.w + x] : 255;
            unsigned bg = ((x / 8 + y / 8) & 1) ? 0xC8 : 0xF0;
            unsigned char rgb[3];
            for (int c = 0; c < 3; c++) {
                unsigned pc = (v >> (16 - 8 * c)) & 255;          /* premultiplied */
                rgb[c] = (unsigned char)(pc + bg * (255 - a) / 255);
            }
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
    img_free(&im);
    free(d);
    return 0;
}
