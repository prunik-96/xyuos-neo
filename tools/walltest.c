/* The desktop's wallpaper, painted on the build machine.
 *
 *     gcc -O2 -Ilibrast/include -o /tmp/walltest tools/walltest.c \
 *         librast/src/math.c librast/src/path.c librast/src/raster.c \
 *         librast/src/stroke.c
 *     /tmp/walltest 1920 1080 0 /tmp/wall.ppm      (the 0: 1 for dark)
 *
 * The same kernel/wm/wall.c and the blur from kernel/wm/ui.c the kernel
 * uses, with the kernel's allocator and framebuffer stood in for -- so the
 * picture can be looked at and tuned in seconds instead of a boot. A second
 * file name writes the frosted copy glass shows. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void *kmalloc(size_t n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }

volatile uint8_t *fb_get_base(void) { return 0; }
uint32_t fb_get_pitch(void) { return 0; }
uint32_t fb_get_width(void) { return 0; }
uint32_t fb_get_height(void) { return 0; }
void fb_mark_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)x; (void)y; (void)w; (void)h;
}
const uint32_t *icon_get(const char *name, int size) { (void)name; (void)size; return 0; }

// On the host there is one core, and the pool just runs the job here.
int  smp_cpu_count(void) { return 1; }
void smp_run(void (*fn)(int, int, void *), void *arg) { fn(0, 1, arg); }
#include "../kernel/wm/ui.c"
#include "../kernel/wm/wall.c"

struct ui_face ui_faces[UI_FACES];       // no text is drawn here
int ui_fonts_ready;

static void save(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char c[3] = { px[i] >> 16, px[i] >> 8, px[i] };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: walltest W H dark out.ppm [frost.ppm]\n");
        return 2;
    }
    int w = atoi(argv[1]), h = atoi(argv[2]), dark = atoi(argv[3]);
    uint32_t *px = malloc((size_t)w * h * 4);
    wall_paint(px, w, h, dark);
    save(argv[4], px, w, h);
    if (argc > 5) {
        uint32_t *fr = malloc((size_t)w * h * 4);
        wall_frost(px, fr, w, h, dark);
        save(argv[5], fr, w, h);
    }
    return 0;
}
