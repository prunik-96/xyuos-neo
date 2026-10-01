// The desktop's face, rasterised once at start-up (see uifont.h).
//
// Built with the floating-point unit on, like the rest of kernel/gfx, and run
// only from wm_start(), before the first program exists. Nothing here may be
// called later: drawing lives in kernel/wm/ui.c, in integers.

#include "uifont.h"
#include "kmath.h"
#include "stb_truetype.h"
#include "../mm/heap.h"
#include "../fs/vfs.h"
#include "../kernel/kio.h"

struct ui_face ui_faces[UI_FACES];
int ui_fonts_ready;

static const struct { int px, bold; } FACES[UI_FACES] = {
    [UI_F11]  = { 11, 0 },
    [UI_F12]  = { 12, 0 },
    [UI_F13]  = { 13, 0 },
    [UI_F13B] = { 13, 1 },
    [UI_F15]  = { 15, 0 },
    [UI_F15B] = { 15, 1 },
    [UI_F20B] = { 20, 1 },
    [UI_F28]  = { 28, 0 },
    [UI_F44]  = { 44, 0 },
};

static uint8_t *load_file(const char *path) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0 || st.is_dir || st.size < 1024) return 0;
    int fd = vfs_open(path);
    if (fd < 0) return 0;
    uint8_t *buf = (uint8_t *)kmalloc(st.size);
    if (!buf) { vfs_close(fd); return 0; }
    uint32_t got = 0;
    while (got < st.size) {
        int32_t n = vfs_read(fd, buf + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    vfs_close(fd);
    if (got != st.size) { kfree(buf); return 0; }
    return buf;
}

static int code_of(int slot) {
    if (slot < UI_BASE_SLOTS) {
        if (slot >= 0x20 && slot <= 0x7E) return slot;
        if (slot >= 128 && slot < 224)    return 0xA0 + (slot - 128);
        if (slot >= 224 && slot < 352)    return 0x100 + (slot - 224);
        if (slot >= 352 && slot < 448)    return 0x400 + (slot - 352);
        return -1;
    }
    return UI_EXTRA[slot - UI_BASE_SLOTS];
}

// Unhinted outlines at 11-15 pixels come out a shade light: the edge pixels
// carry the right coverage, but straight onto the screen that reads as thin,
// washed-out text. A gentle curve on the coverage -- more at small sizes,
// where most of a stem IS edge -- gives the stems back their weight.
static void make_curve(uint8_t lut[256], int px) {
    double g = px <= 13 ? 0.78 : (px <= 20 ? 0.86 : 0.95);
    for (int i = 0; i < 256; i++) {
        double v = 255.0 * k_pow(i / 255.0, g) + 0.5;
        lut[i] = (uint8_t)(v > 255 ? 255 : v);
    }
}

static int build_face(struct ui_face *F, const stbtt_fontinfo *info, int px) {
    float scale = stbtt_ScaleForMappingEmToPixels(info, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
    F->px = px;
    F->ascent = (int)k_floor(asc * scale + 0.5);
    F->height = F->ascent + (int)k_floor(-desc * scale + 0.5);

    uint8_t lut[256];
    make_curve(lut, px);

    // Sizes first, so the pool is one allocation.
    uint32_t total = 0;
    for (int s = 0; s < UI_SLOTS; s++) {
        struct ui_glyph *g = &F->g[s];
        g->present = 0;
        int cp = code_of(s);
        if (cp < 0 || !stbtt_FindGlyphIndex(info, cp)) continue;
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(info, cp, scale, scale, &x0, &y0, &x1, &y1);
        int a = (int)k_floor(adv * scale + 0.5);
        g->adv = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
        g->w = (uint8_t)((x1 > x0 && x1 - x0 < 256) ? x1 - x0 : 0);
        g->h = (uint8_t)((y1 > y0 && y1 - y0 < 256) ? y1 - y0 : 0);
        if (!g->w || !g->h) g->w = g->h = 0;
        g->xoff = (int8_t)x0;
        g->yoff = (int8_t)y0;
        g->off = total;
        g->present = 1;
        total += (uint32_t)g->w * g->h;
    }
    F->pool = (uint8_t *)kmalloc(total ? total : 1);
    if (!F->pool) return 0;

    for (int s = 0; s < UI_SLOTS; s++) {
        struct ui_glyph *g = &F->g[s];
        if (!g->present || !g->w) continue;
        uint8_t *dst = F->pool + g->off;
        stbtt_MakeCodepointBitmap(info, dst, g->w, g->h, g->w, scale, scale, code_of(s));
        for (uint32_t k = 0; k < (uint32_t)g->w * g->h; k++) dst[k] = lut[dst[k]];
    }
    return 1;
}

int uifont_init(void) {
    static const char *files[2] = {
        "/fonts/NotoSans-Regular.ttf", "/fonts/NotoSans-Bold.ttf",
    };
    uint8_t *data[2];
    stbtt_fontinfo info[2];
    for (int w = 0; w < 2; w++) {
        data[w] = load_file(files[w]);
        if (!data[w] || !stbtt_InitFont(&info[w], data[w], stbtt_GetFontOffsetForIndex(data[w], 0))) {
            kprintf("uifont: cannot use %s\n", files[w]);
            if (w == 0) return 0;
            info[1] = info[0];        // no bold: the regular face stands in
            data[1] = 0;
        }
    }
    for (int f = 0; f < UI_FACES; f++) {
        if (!build_face(&ui_faces[f], &info[FACES[f].bold], FACES[f].px)) {
            kprintf("uifont: out of memory at face %d\n", f);
            return 0;
        }
    }
    // The outlines are not needed once every glyph is a bitmap.
    kfree(data[0]);
    if (data[1]) kfree(data[1]);
    ui_fonts_ready = 1;
    kprintf("uifont: Noto Sans, %d faces\n", UI_FACES);
    return 1;
}
