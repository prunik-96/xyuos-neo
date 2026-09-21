#include "icons.h"
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../drivers/framebuffer.h"
#include "../kernel/kio.h"

// One file, read once, kept as-is. The build step already did the decoding,
// the background removal and the scaling, so there is nothing to do here but
// find the right offset and blend.
//
// Format (little-endian):
//   "XICO", u32 count, u32 big, u32 small
//   count * { char name[24]; u32 off_big; u32 off_small; }
//   pixels: for each icon, big*big words then small*small words, 0xAARRGGBB

#define ICON_NAME 24

struct icon_ent {
    char     name[ICON_NAME];
    uint32_t off_big, off_small;
};

static uint8_t         *blob;
static uint32_t         blob_len;
static struct icon_ent *ents;
static uint32_t         nents;
static const uint32_t  *pixels;      // the pixel area, as words

static int name_eq(const char *a, const char *b) {
    int i = 0;
    for (; i < ICON_NAME; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

int icons_load(void) {
    if (blob) return 1;
    int fd = vfs_open("/icons.bin");
    if (fd < 0) return 0;

    struct vfs_stat st;
    if (vfs_fstat(fd, &st) != 0 || st.size < 16) { vfs_close(fd); return 0; }

    blob = (uint8_t *)kmalloc((uint32_t)st.size);
    if (!blob) { vfs_close(fd); return 0; }

    uint32_t got = 0;
    while (got < st.size) {
        int32_t n = vfs_read(fd, blob + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    vfs_close(fd);

    if (got != st.size || blob[0] != 'X' || blob[1] != 'I' ||
        blob[2] != 'C' || blob[3] != 'O') {
        kfree(blob); blob = 0; return 0;
    }
    blob_len = got;

    uint32_t *hdr = (uint32_t *)(blob + 4);
    nents = hdr[0];
    uint32_t big = hdr[1], small = hdr[2];
    if (big != ICON_BIG || small != ICON_SMALL || nents > 512) {
        kprintf("icons: /icons.bin is not the shape this build expects\n");
        kfree(blob); blob = 0; return 0;
    }

    ents = (struct icon_ent *)(blob + 16);
    pixels = (const uint32_t *)(blob + 16 + nents * sizeof(struct icon_ent));
    kprintf("icons: %u loaded\n", nents);
    return 1;
}

const uint32_t *icon_get(const char *name, int size) {
    if (!blob || !name) return 0;
    for (uint32_t i = 0; i < nents; i++) {
        if (!name_eq(ents[i].name, name)) continue;
        uint32_t off = (size == ICON_SMALL) ? ents[i].off_small : ents[i].off_big;
        return (const uint32_t *)((const uint8_t *)pixels + off);
    }
    return 0;
}

int icon_draw(int x, int y, const char *name, int size) {
    const uint32_t *src = icon_get(name, size);
    if (!src) return 0;

    uint8_t *base = (uint8_t *)fb_get_base();
    if (!base) return 0;
    uint32_t pitch = fb_get_pitch(), fw = fb_get_width(), fh = fb_get_height();

    for (int j = 0; j < size; j++) {
        int py = y + j;
        if (py < 0 || (uint32_t)py >= fh) continue;
        uint32_t *row = (uint32_t *)(base + (size_t)py * pitch);
        for (int i = 0; i < size; i++) {
            int px = x + i;
            if (px < 0 || (uint32_t)px >= fw) continue;
            uint32_t s = src[j * size + i];
            uint32_t a = s >> 24;
            if (!a) continue;                       // fully clear: leave it
            if (a == 255) { row[px] = s & 0x00FFFFFF; continue; }
            uint32_t d = row[px];
            uint32_t r = (((s >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            uint32_t g = (((s >> 8)  & 0xFF) * a + ((d >> 8)  & 0xFF) * (255 - a)) / 255;
            uint32_t b = (((s)       & 0xFF) * a + ((d)       & 0xFF) * (255 - a)) / 255;
            row[px] = (r << 16) | (g << 8) | b;
        }
    }
    fb_mark_rect((uint32_t)(x < 0 ? 0 : x), (uint32_t)(y < 0 ? 0 : y),
                 (uint32_t)size, (uint32_t)size);
    return 1;
}
