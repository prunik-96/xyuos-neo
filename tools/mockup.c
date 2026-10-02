/* Mock-ups of the desktop, drawn on the build machine.
 *
 * The same drawing code the kernel uses -- kernel/wm/ui.c, wall.c, the
 * Noto Sans cache in gfx/uifont.c, librast -- with the kernel's allocator,
 * file system and framebuffer stood in for, so a whole screen can be looked
 * at in seconds and argued about before a line of the window manager
 * changes. Run from the repository root:
 *
 *     tools/mockup.sh            (builds this and writes ~/dbg/mock_*.png)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

void *kmalloc(size_t n) { return malloc(n ? n : 1); }
void  kfree(void *p) { free(p); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }

volatile uint8_t *fb_get_base(void) { return 0; }
uint32_t fb_get_pitch(void) { return 0; }
uint32_t fb_get_width(void) { return 0; }
uint32_t fb_get_height(void) { return 0; }
void fb_mark_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)x; (void)y; (void)w; (void)h; }
const uint32_t *icon_get(const char *name, int size) { (void)name; (void)size; return 0; }

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* The kernel's file system, as far as the font loader needs it: /fonts/x
 * is assets/fonts/x here. */
#include "../kernel/fs/vfs.h"
static FILE *files[8];
static void host_path(const char *p, char *out, size_t n) {
    if (!strncmp(p, "/fonts/", 7)) snprintf(out, n, "assets%s", p);
    else snprintf(out, n, "%s", p);
}
int vfs_stat(const char *path, struct vfs_stat *st) {
    char hp[256];
    host_path(path, hp, sizeof hp);
    FILE *f = fopen(hp, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    st->size = (uint32_t)ftell(f);
    st->is_dir = 0;
    fclose(f);
    return 0;
}
int vfs_open(const char *path) {
    char hp[256];
    host_path(path, hp, sizeof hp);
    for (int i = 0; i < 8; i++) if (!files[i]) { files[i] = fopen(hp, "rb"); return files[i] ? i : -1; }
    return -1;
}
int32_t vfs_read(int fd, void *buf, uint32_t len) { return (int32_t)fread(buf, 1, len, files[fd]); }
void vfs_close(int fd) { fclose(files[fd]); files[fd] = 0; }

// On the host there is one core, and the pool just runs the job here.
int  smp_cpu_count(void) { return 1; }
void smp_run(void (*fn)(int, int, void *), void *arg) { fn(0, 1, arg); }
#include "../kernel/wm/ui.c"
#include "../kernel/wm/wall.c"
#include "../kernel/gfx/uifont.c"
#include "../kernel/wm/cursor.h"

/* ------------------------------------------------------------------------ */

#define W 1920
#define H 1080
static uint32_t *cv;                 /* the canvas */
static uint32_t ACC;                 /* the accent, from the wallpaper */
static struct ui_face mono13;

static void save_png(const char *path) {
    char ppm[256];
    snprintf(ppm, sizeof ppm, "%s.ppm", path);
    FILE *f = fopen(ppm, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char c[3] = { cv[i] >> 16, cv[i] >> 8, cv[i] };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

static uint32_t mixc(uint32_t a, uint32_t b, int t) { return ui_mix(a, b, t); }

/* --- shapes, for icons and glyphs ---------------------------------------- */

#define FXI(v) RAST_INT(v)
#define FXF(n, d) RAST_FRAC(n, d)

static void fillp(rast_path *p, uint32_t argb) {
    rast_paint pt;
    rast_paint_solid(&pt, argb);
    ui_rast_fill(p, &pt);
}
static void strokep(rast_path *p, rast_fx wdt, uint32_t argb) {
    rast_stroke s;
    rast_stroke_init(&s, wdt);
    s.cap = RAST_CAP_ROUND;
    s.join = RAST_JOIN_ROUND;
    rast_paint pt;
    rast_paint_solid(&pt, argb);
    ui_rast_stroke(p, &s, &pt);
}
static void vgradp(rast_path *p, int y0, int y1, uint32_t c0, uint32_t c1) {
    rast_stop st[2] = { { 0, c0 }, { RAST_ONE, c1 } };
    rast_paint pt;
    memset(&pt, 0, sizeof pt);
    pt.type = RAST_LINEAR; pt.m = rast_identity();
    pt.x1 = 0; pt.y1 = FXI(y0); pt.x2 = 0; pt.y2 = FXI(y1);
    pt.stops = st; pt.nstops = 2; pt.opacity = 255; pt.spread = RAST_PAD;
    ui_rast_fill(p, &pt);
}

/* Soft filled glyphs, drawn into a box of size s at (x, y), in colour c. */
enum { G_FOLDER, G_GLOBE, G_TERM, G_NOTE, G_MUSIC, G_GEAR, G_CHART, G_IMAGE, G_POWER,
       G_SEARCH, G_WIFI, G_VOL, G_PIN, G_MIN, G_MAX, G_CLOSE, G_PLUS, G_MOON, G_BELL,
       G_SAVE, G_PLAY, G_NEXT, G_PREV, G_DOC, G_CODE };

static uint32_t GINNER = 0xFFFFFFFF;    /* the colour of a glyph's inner details */
static void glyph(int g, int x, int y, int s, uint32_t c) {
    rast_path p;
    rast_path_init(&p);
    rast_fx X = FXI(x), Y = FXI(y), S = FXI(s);
#define PT(a, b) X + (rast_fx)((int64_t)S * (a) / 100), Y + (rast_fx)((int64_t)S * (b) / 100)
#define LEN(a) (rast_fx)((int64_t)S * (a) / 100)
    switch (g) {
    case G_FOLDER:
        rast_round_rect(&p, PT(8, 22), LEN(84), LEN(64), LEN(10), LEN(10));
        rast_round_rect(&p, PT(8, 14), LEN(36), LEN(20), LEN(8), LEN(8));
        fillp(&p, c);
        break;
    case G_GLOBE:
        rast_ellipse(&p, PT(50, 50), LEN(40), LEN(40));
        strokep(&p, LEN(9), c);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(50, 50), LEN(16), LEN(40));
        rast_move_to(&p, PT(12, 50)); rast_line_to(&p, PT(88, 50));
        strokep(&p, LEN(7), c);
        break;
    case G_TERM:
        rast_round_rect(&p, PT(6, 14), LEN(88), LEN(72), LEN(14), LEN(14));
        fillp(&p, c);
        rast_path_reset(&p);
        rast_move_to(&p, PT(24, 36)); rast_line_to(&p, PT(40, 50)); rast_line_to(&p, PT(24, 64));
        rast_move_to(&p, PT(48, 66)); rast_line_to(&p, PT(70, 66));
        strokep(&p, LEN(8), GINNER);
        break;
    case G_NOTE: case G_DOC:
        rast_round_rect(&p, PT(16, 6), LEN(68), LEN(88), LEN(12), LEN(12));
        fillp(&p, c);
        rast_path_reset(&p);
        for (int k = 0; k < 4; k++) { rast_move_to(&p, PT(30, 30 + k * 14)); rast_line_to(&p, PT(k == 3 ? 52 : 70, 30 + k * 14)); }
        strokep(&p, LEN(6), GINNER);
        break;
    case G_MUSIC: case G_PLAY:
        if (g == G_PLAY) {
            rast_move_to(&p, PT(30, 18)); rast_line_to(&p, PT(82, 50)); rast_line_to(&p, PT(30, 82));
            rast_close(&p);
            fillp(&p, c);
            strokep(&p, LEN(10), c);
            break;
        }
        rast_ellipse(&p, PT(30, 74), LEN(16), LEN(13));
        rast_ellipse(&p, PT(74, 64), LEN(16), LEN(13));
        fillp(&p, c);
        rast_path_reset(&p);
        rast_move_to(&p, PT(42, 74)); rast_line_to(&p, PT(42, 20)); rast_line_to(&p, PT(86, 12));
        rast_line_to(&p, PT(86, 64));
        strokep(&p, LEN(9), c);
        break;
    case G_GEAR:
        for (int k = 0; k < 8; k++) {
            rast_fx a = RAST_INT(k * 45);
            rast_fx ca = rast_cos(a), sa = rast_sin(a);
            rast_move_to(&p, X + S / 2 + rast_mulfx(ca, LEN(28)), Y + S / 2 + rast_mulfx(sa, LEN(28)));
            rast_line_to(&p, X + S / 2 + rast_mulfx(ca, LEN(42)), Y + S / 2 + rast_mulfx(sa, LEN(42)));
        }
        strokep(&p, LEN(16), c);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(50, 50), LEN(30), LEN(30));
        fillp(&p, c);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(50, 50), LEN(12), LEN(12));
        fillp(&p, GINNER);
        break;
    case G_CHART:
        rast_round_rect(&p, PT(10, 50), LEN(18), LEN(40), LEN(6), LEN(6));
        rast_round_rect(&p, PT(41, 26), LEN(18), LEN(64), LEN(6), LEN(6));
        rast_round_rect(&p, PT(72, 10), LEN(18), LEN(80), LEN(6), LEN(6));
        fillp(&p, c);
        break;
    case G_IMAGE:
        rast_round_rect(&p, PT(6, 14), LEN(88), LEN(72), LEN(14), LEN(14));
        fillp(&p, c);
        rast_path_reset(&p);
        rast_move_to(&p, PT(14, 76)); rast_line_to(&p, PT(40, 46)); rast_line_to(&p, PT(58, 64));
        rast_line_to(&p, PT(70, 54)); rast_line_to(&p, PT(88, 76)); rast_close(&p);
        fillp(&p, GINNER);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(70, 34), LEN(8), LEN(8));
        fillp(&p, GINNER);
        break;
    case G_POWER:
        rast_arc(&p, PT(50, 54), LEN(32), RAST_INT(-55), RAST_INT(235));
        strokep(&p, LEN(11), c);
        rast_path_reset(&p);
        rast_move_to(&p, PT(50, 12)); rast_line_to(&p, PT(50, 48));
        strokep(&p, LEN(11), c);
        break;
    case G_SEARCH:
        rast_ellipse(&p, PT(42, 42), LEN(26), LEN(26));
        strokep(&p, LEN(11), c);
        rast_path_reset(&p);
        rast_move_to(&p, PT(62, 62)); rast_line_to(&p, PT(84, 84));
        strokep(&p, LEN(13), c);
        break;
    case G_WIFI:
        for (int k = 0; k < 3; k++) {
            p.open = 0;
            rast_arc(&p, PT(50, 82), LEN(22 + k * 22), RAST_INT(-135), RAST_INT(-45));
        }
        strokep(&p, LEN(11), c);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(50, 80), LEN(8), LEN(8));
        fillp(&p, c);
        break;
    case G_VOL:
        rast_move_to(&p, PT(10, 36)); rast_line_to(&p, PT(28, 36)); rast_line_to(&p, PT(52, 14));
        rast_line_to(&p, PT(52, 86)); rast_line_to(&p, PT(28, 64)); rast_line_to(&p, PT(10, 64));
        rast_close(&p);
        fillp(&p, c);
        strokep(&p, LEN(6), c);
        rast_path_reset(&p);
        p.open = 0; rast_arc(&p, PT(54, 50), LEN(18), RAST_INT(-45), RAST_INT(45));
        p.open = 0; rast_arc(&p, PT(54, 50), LEN(34), RAST_INT(-45), RAST_INT(45));
        strokep(&p, LEN(9), c);
        break;
    case G_PIN:
        rast_round_rect(&p, PT(30, 8), LEN(40), LEN(40), LEN(10), LEN(10));
        rast_move_to(&p, PT(16, 52)); rast_line_to(&p, PT(84, 52)); rast_line_to(&p, PT(70, 38));
        rast_line_to(&p, PT(30, 38)); rast_close(&p);
        fillp(&p, c);
        rast_path_reset(&p);
        rast_move_to(&p, PT(50, 52)); rast_line_to(&p, PT(50, 92));
        strokep(&p, LEN(9), c);
        break;
    case G_MIN:
        rast_move_to(&p, PT(22, 50)); rast_line_to(&p, PT(78, 50));
        strokep(&p, LEN(11), c);
        break;
    case G_MAX:
        rast_round_rect(&p, PT(22, 22), LEN(56), LEN(56), LEN(14), LEN(14));
        strokep(&p, LEN(10), c);
        break;
    case G_CLOSE:
        rast_move_to(&p, PT(24, 24)); rast_line_to(&p, PT(76, 76));
        rast_move_to(&p, PT(76, 24)); rast_line_to(&p, PT(24, 76));
        strokep(&p, LEN(11), c);
        break;
    case G_PLUS:
        rast_move_to(&p, PT(50, 20)); rast_line_to(&p, PT(50, 80));
        rast_move_to(&p, PT(20, 50)); rast_line_to(&p, PT(80, 50));
        strokep(&p, LEN(10), c);
        break;
    case G_MOON:
        rast_ellipse(&p, PT(50, 50), LEN(38), LEN(38));
        fillp(&p, c);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(66, 36), LEN(30), LEN(30));
        fillp(&p, GINNER);                         /* cut by drawing the tile's colour over */
        break;
    case G_BELL:
        rast_move_to(&p, PT(18, 74)); rast_cubic_to(&p, PT(26, 64), PT(22, 18), PT(50, 18));
        rast_cubic_to(&p, PT(78, 18), PT(74, 64), PT(82, 74)); rast_close(&p);
        fillp(&p, c);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(50, 84), LEN(9), LEN(8));
        fillp(&p, c);
        break;
    case G_SAVE:
        rast_round_rect(&p, PT(10, 10), LEN(80), LEN(80), LEN(16), LEN(16));
        fillp(&p, c);
        rast_path_reset(&p);
        rast_round_rect(&p, PT(28, 52), LEN(44), LEN(30), LEN(6), LEN(6));
        rast_round_rect(&p, PT(30, 18), LEN(34), LEN(20), LEN(5), LEN(5));
        fillp(&p, GINNER);
        break;
    case G_NEXT: case G_PREV: {
        int f = g == G_NEXT;
        rast_move_to(&p, PT(f ? 18 : 82, 22)); rast_line_to(&p, PT(f ? 62 : 38, 50));
        rast_line_to(&p, PT(f ? 18 : 82, 78)); rast_close(&p);
        rast_round_rect(&p, PT(f ? 66 : 18, 22), LEN(14), LEN(56), LEN(4), LEN(4));
        fillp(&p, c);
        break;
    }
    case G_CODE:
        rast_move_to(&p, PT(36, 26)); rast_line_to(&p, PT(14, 50)); rast_line_to(&p, PT(36, 74));
        rast_move_to(&p, PT(64, 26)); rast_line_to(&p, PT(86, 50)); rast_line_to(&p, PT(64, 74));
        strokep(&p, LEN(11), c);
        break;
    }
#undef PT
#undef LEN
    rast_path_free(&p);
}

/* An application's icon: a soft rounded square in its colour, lit from the
 * top, with its glyph in white. Until the drawn ones arrive. */
static void app_icon(int x, int y, int s, uint32_t col, int g) {
    rast_path p;
    rast_path_init(&p);
    int r = s * 28 / 100;
    rast_round_rect(&p, FXI(x), FXI(y), FXI(s), FXI(s), FXI(r), FXI(r));
    vgradp(&p, y, y + s, 0xFF000000 | mixc(col, 0xFFFFFF, 50), 0xFF000000 | mixc(col, 0, 20));
    rast_path_free(&p);
    ui_rrect_line(x, y, s, s, r, UI_ALL, 0x00FFFFFF, 70);
    int gs = s * 56 / 100;
    GINNER = 0xFF000000 | mixc(col, 0, 10);
    glyph(g, x + (s - gs) / 2, y + (s - gs) / 2, gs, 0xFFFFFFFF);
    GINNER = 0xFFFFFFFF;
}

/* --- the parts of the desktop ---------------------------------------------- */

static uint32_t INK = 0x1E2329, DIM = 0x6B7480, BODY = 0xFBFCFD;

static void win_shadow(int x, int y, int w, int h, int r, int strong) {
    ui_shadow(x, y, w, h, r, 34, strong ? 70 : 50, 14, 0);
    ui_shadow(x, y, w, h, r, 4, strong ? 45 : 30, 1, 0);
}

/* Text in a face, n pixels from x, vertically centred on cy. */
static int text_c(int x, int cy, const char *s, int face, uint32_t col) {
    return ui_text(x, cy - ui_line_h(face) / 2, s, face, col);
}

typedef struct { const char *title; int glyph; uint32_t col; } tab_t;

#define TAB_H 44

/* A window whose top is its tabs. The strip is coloured glass; the active
 * tab is a white shape that runs on into the body below, the way a tab of a
 * paper folder does. At the right: pin, minimise, maximise, close as soft
 * round buttons. Returns the content rectangle. */
static void window(int x, int y, int w, int h, const tab_t *tabs, int ntabs, int active,
                   int focus, int hot_btn, int pinned, int *cx, int *cy, int *cw, int *ch) {
    const int R = 18;
    win_shadow(x, y, w, h, R, focus);
    ui_glass_live(x, y, w, h, R, mixc(0xFFFFFF, ACC, 34), focus ? 150 : 185, 22);

    /* The body: white, under the tab strip. */
    ui_mat body = { UI_SOLID, BODY, 0, 255, 0, 0, 0 };
    ui_rrect2(x + 6, y + TAB_H, w - 12, h - TAB_H - 6, 12, 12, &body);

    /* Tabs. */
    int tx = x + 10;
    for (int i = 0; i < ntabs; i++) {
        int tw = 38 + ui_text_w(tabs[i].title, i == active ? UI_F13B : UI_F13) + (i == active ? 40 : 18);
        if (tw > 240) tw = 240;
        if (i == active) {
            /* A white tab joined to the body, with flared feet. */
            ui_rrect(tx, y + 7, tw, TAB_H - 7 + 2, 12, UI_TOP, &body);
            rast_path p;
            rast_path_init(&p);
            int by = y + TAB_H;
            rast_move_to(&p, FXI(tx - 10), FXI(by));
            rast_quad_to(&p, FXI(tx), FXI(by), FXI(tx), FXI(by - 10));
            rast_line_to(&p, FXI(tx), FXI(by)); rast_close(&p);
            rast_move_to(&p, FXI(tx + tw + 10), FXI(by));
            rast_quad_to(&p, FXI(tx + tw), FXI(by), FXI(tx + tw), FXI(by - 10));
            rast_line_to(&p, FXI(tx + tw), FXI(by)); rast_close(&p);
            fillp(&p, 0xFF000000 | BODY);
            rast_path_free(&p);
        }
        app_icon(tx + 12, y + 7 + (TAB_H - 7 - 18) / 2, 18, tabs[i].col, tabs[i].glyph);
        int saved[4];
        ui_clip_get(saved);
        ui_clip(tx, y, tw - (i == active ? 34 : 10), TAB_H);
        text_c(tx + 38, y + 7 + (TAB_H - 7) / 2, tabs[i].title, i == active ? UI_F13B : UI_F13,
               i == active ? INK : (focus ? 0x3A4250 : DIM));
        ui_clip_set(saved);
        if (i == active) glyph(G_CLOSE, tx + tw - 26, y + 7 + (TAB_H - 7) / 2 - 7, 14, 0xFF000000 | DIM);
        tx += tw + 4;
    }
    /* New tab. */
    ui_round_fill(tx + 4, y + 13, 26, 26, 13, 0xFFFFFF, 90);
    glyph(G_PLUS, tx + 10, y + 19, 14, 0xFF000000 | INK);

    /* Buttons. */
    static const int btns[4] = { G_PIN, G_MIN, G_MAX, G_CLOSE };
    int bx = x + w - 10 - 4 * 34;
    for (int b = 0; b < 4; b++) {
        int cxb = bx + b * 34, cyb = y + 10;
        uint32_t fill = 0xFFFFFF;
        int a = 120;
        uint32_t ink = 0xFF000000 | INK;
        if (b == 3 && hot_btn == 3) { fill = 0xE5484D; a = 255; ink = 0xFFFFFFFF; }
        else if (b == hot_btn) a = 230;
        if (b == 0 && pinned) { fill = ACC; a = 255; ink = 0xFFFFFFFF; }
        ui_round_fill(cxb, cyb, 28, 28, 14, fill, a);
        glyph(btns[b], cxb + 7, cyb + 7, 14, ink);
    }

    *cx = x + 6; *cy = y + TAB_H; *cw = w - 12; *ch = h - TAB_H - 6;
}

/* Window contents, for the pictures. */
static void files_body(int x, int y, int w, int h) {
    ui_round_fill(x + 8, y + 8, 200, h - 16, 10, mixc(0xFFFFFF, ACC, 16), 255);
    static const char *places[] = { "Главная", "Документы", "Картинки", "Музыка", "Загрузки", "Флешка" };
    static const int pg[] = { G_FOLDER, G_DOC, G_IMAGE, G_MUSIC, G_FOLDER, G_SAVE };
    for (int i = 0; i < 6; i++) {
        int ry = y + 22 + i * 38;
        if (i == 1) ui_round_fill(x + 16, ry - 6, 184, 34, 9, ACC, 255);
        GINNER = 0xFF000000 | (i == 1 ? ACC : mixc(0xFFFFFF, ACC, 16));
        glyph(pg[i], x + 28, ry + 2, 18, i == 1 ? 0xFFFFFFFF : (0xFF000000 | mixc(ACC, 0, 40)));
        GINNER = 0xFFFFFFFF;
        text_c(x + 56, ry + 11, places[i], UI_F13, i == 1 ? 0xFFFFFF : INK);
    }
    int lx = x + 224;
    ui_round_fill(lx, y + 12, w - 240, 34, 17, 0xEEF1F4, 255);
    glyph(G_SEARCH, lx + 12, y + 21, 16, 0xFF000000 | DIM);
    text_c(lx + 38, y + 29, "Искать в «Документах»", UI_F13, DIM);
    static const char *names[] = { "Отчёт за сентябрь.txt", "Фото с прогулки", "Заметки.md", "demo.mp3",
                                   "калькулятор.c", "Ссылки.html" };
    static const char *meta[] = { "сегодня, 14:02", "вчера", "28 сентября", "21 секунда",
                                  "26 сентября", "12 сентября" };
    static const int ng[] = { G_DOC, G_IMAGE, G_NOTE, G_MUSIC, G_CODE, G_GLOBE };
    static const uint32_t nc[] = { 0x4C7BD9, 0x3BA776, 0xE0A23B, 0xE06A4C, 0x7A5BD6, 0x2F9BD0 };
    for (int i = 0; i < 6 && 64 + i * 52 < h - 20; i++) {
        int ry = y + 62 + i * 52;
        if (i == 0) ui_round_fill(lx, ry - 4, w - 240, 46, 12, mixc(0xFFFFFF, ACC, 30), 255);
        app_icon(lx + 10, ry + 3, 32, nc[i], ng[i]);
        text_c(lx + 56, ry + 12, names[i], UI_F13, INK);
        text_c(lx + 56, ry + 30, meta[i], UI_F11, DIM);
    }
}

static void term_body(int x, int y, int w, int h) {
    ui_rrect2(x, y, w, h, 0, 12, &(ui_mat){ UI_SOLID, 0x1C2026, 0, 255, 0, 0, 0 });
    struct ui_face save = ui_faces[UI_F13];
    ui_faces[UI_F13] = mono13;
    static const char *lines[] = {
        "xyuOS Neo shell. 'help' for commands.",
        "dyx:/$ ls /sounds",
        "demo.mp3  demo.ogg  chime.wav",
        "dyx:/$ play /sounds/demo.mp3 &",
        "[1] 14",
        "dyx:/$ cc calc.c -o calc && ./calc",
        "2 + 2 * 2 = 6",
        "dyx:/$ _",
    };
    static const uint32_t col[] = { 0x9BA7B4, 0x8FD0A0, 0xE6EAF0, 0x8FD0A0, 0x9BA7B4, 0x8FD0A0,
                                    0xE6EAF0, 0x8FD0A0 };
    for (int i = 0; i < 8 && 14 + i * 22 < h - 10; i++)
        ui_text(x + 16, y + 12 + i * 22, lines[i], UI_F13, col[i]);
    ui_faces[UI_F13] = save;
}

static void browser_body(int x, int y, int w, int h) {
    ui_round_fill(x + 10, y + 10, w - 20, 36, 18, 0xEEF1F4, 255);
    glyph(G_PREV, x + 24, y + 21, 14, 0xFF000000 | DIM);
    glyph(G_NEXT, x + 48, y + 21, 14, 0xFF000000 | 0xB0B7C0);
    text_c(x + 80, y + 28, "xyuos.org/news", UI_F13, INK);
    /* A page: a header picture, a headline, paragraphs as soft bars. */
    int py = y + 60;
    ui_round_grad(x + 24, py, w - 48, 150, 14, UI_ALL, mixc(ACC, 0xFFFFFF, 90), 255, ACC, 255);
    ui_text(x + 48, py + 40, "Новый рабочий стол", UI_F28, 0xFFFFFF);
    ui_text(x + 50, py + 84, "спокойный, свой и с цветным стеклом", UI_F15, 0xF4F6FA);
    for (int i = 0; i < 6 && py + 180 + i * 26 < y + h - 20; i++)
        ui_round_fill(x + 24, py + 176 + i * 26, (w - 48) * (i == 5 ? 55 : 92 + (i & 1) * 6) / 100, 10, 5,
                      0xDDE2E8, 255);
}

static void notes_body(int x, int y, int w, int h) {
    (void)h;
    ui_text(x + 24, y + 18, "Список дел", UI_F20B, INK);
    static const char *l[] = { "• починить призраки у значков", "• окна вкладками",
                               "• остров можно перенести к краю", "• закрепить плеер поверх",
                               "• сохранить раскладку стола" };
    for (int i = 0; i < 5; i++) ui_text(x + 26, y + 60 + i * 28, l[i], UI_F15, INK);
    (void)w;
}

/* The island: the panel, floating. Bottom (horizontal) or left (vertical). */
#define ISL 64
static void island(int vertical, int show_launcher_lit, int ctl_lit) {
    static const struct { int g; uint32_t col; int run, active; } apps[] = {
        { G_FOLDER, 0x3E7BE0, 1, 1 }, { G_GLOBE, 0x2F9BD0, 1, 0 }, { G_TERM, 0x2A3038, 1, 0 },
        { G_NOTE, 0xE0A23B, 1, 0 }, { G_MUSIC, 0xE06A4C, 1, 0 }, { G_CHART, 0x3BA776, 0, 0 },
        { G_GEAR, 0x6E7887, 0, 0 },
    };
    int n = (int)(sizeof apps / sizeof apps[0]);
    int item = 52;
    int len = 12 + 52 + 14 + n * item + 14 + 176;
    int x, y, w, h;
    if (vertical) { w = ISL; h = len + 40; x = 14; y = (H - h) / 2; }
    else { w = len; h = ISL; x = (W - w) / 2; y = H - h - 14; }
    win_shadow(x, y, w, h, 26, 1);
    ui_glass_live(x, y, w, h, 26, mixc(0xFFFFFF, ACC, 60), 135, 24);

    /* The launcher button: the logo, in the accent. */
    int lx = vertical ? x + 10 : x + 10, ly = vertical ? y + 10 : y + 8;
    rast_path p;
    rast_path_init(&p);
    rast_ellipse(&p, FXI(lx + 22), FXI(ly + 22) + (vertical ? 0 : RAST_ONE), FXI(22), FXI(22));
    vgradp(&p, ly, ly + 44, 0xFF000000 | mixc(ACC, 0xFFFFFF, 70), 0xFF000000 | mixc(ACC, 0, 30));
    rast_path_free(&p);
    if (show_launcher_lit) ui_rrect_line(lx - 3, ly - 3 + (vertical ? 0 : 1), 50, 50, 25, UI_ALL, 0xFFFFFF, 220);
    /* The mark: a ring with a moon riding it. */
    rast_path_init(&p);
    rast_ellipse(&p, FXI(lx + 22), FXI(ly + 23), FXI(10), FXI(10));
    strokep(&p, FXI(4), 0xFFFFFFFF);
    rast_path_reset(&p);
    rast_ellipse(&p, FXI(lx + 31), FXI(ly + 15), FXI(4), FXI(4));
    fillp(&p, 0xFFFFFFFF);
    rast_path_free(&p);

    for (int i = 0; i < n; i++) {
        int ix = vertical ? x + (w - 44) / 2 : x + 12 + 52 + 14 + i * item;
        int iy = vertical ? y + 12 + 52 + 14 + i * item : y + (h - 44) / 2 - 2;
        if (apps[i].active) ui_round_fill(ix - 4, iy - 4, 52, 52, 16, 0xFFFFFF, 150);
        app_icon(ix, iy, 44, apps[i].col, apps[i].g);
        if (apps[i].run) {
            int dw = apps[i].active ? 14 : 5;
            if (vertical) ui_round_fill(x + 3, iy + 22 - dw / 2, 4, dw, 2, apps[i].active ? ACC : 0x5A6270, 255);
            else ui_round_fill(ix + 22 - dw / 2, y + h - 7, dw, 4, 2, apps[i].active ? ACC : 0x5A6270, 255);
        }
    }

    /* The status pill: network, sound, time. */
    if (!vertical) {
        int sx = x + w - 10 - 176, sy = y + 10, sw = 176, sh = h - 20;
        ui_round_fill(sx, sy, sw, sh, sh / 2, 0xFFFFFF, ctl_lit ? 230 : 120);
        glyph(G_WIFI, sx + 14, sy + 13, 18, 0xFF000000 | INK);
        glyph(G_VOL, sx + 42, sy + 13, 18, 0xFF000000 | INK);
        ui_text(sx + 76, sy + 4, "14:32", UI_F15B, INK);
        ui_text(sx + 76, sy + 23, "Чт, 1 окт", UI_F11, DIM);
    } else {
        int sy = y + h - 116;
        glyph(G_WIFI, x + 22, sy + 4, 20, 0xFF000000 | INK);
        glyph(G_VOL, x + 22, sy + 34, 20, 0xFF000000 | INK);
        int tw = ui_text_w("14", UI_F15B);
        ui_text(x + (w - tw) / 2, sy + 64, "14", UI_F15B, INK);
        tw = ui_text_w("32", UI_F15B);
        ui_text(x + (w - tw) / 2, sy + 84, "32", UI_F15B, INK);
    }
}

/* The launcher: rises out of the island. Search, the programs pinned to it,
 * what was opened lately, and you and the power button at the foot. */
static void launcher(void) {
    int w = 640, h = 560, x = (W - 774) / 2, y = H - ISL - 14 - 14 - h;
    win_shadow(x, y, w, h, 24, 1);
    ui_glass_live(x, y, w, h, 24, mixc(0xFFFFFF, ACC, 40), 175, 26);

    ui_round_fill(x + 24, y + 24, w - 48, 44, 22, 0xFFFFFF, 235);
    glyph(G_SEARCH, x + 40, y + 37, 18, 0xFF000000 | DIM);
    text_c(x + 70, y + 46, "Программы, файлы, настройки…", UI_F15, DIM);

    ui_text(x + 28, y + 88, "Закреплённые", UI_F13B, INK);
    ui_text(x + w - 28 - ui_text_w("Все программы", UI_F12), y + 88, "Все программы", UI_F12, mixc(ACC, 0, 40));
    static const struct { const char *n; int g; uint32_t c; } pins[] = {
        { "Файлы", G_FOLDER, 0x3E7BE0 }, { "Браузер", G_GLOBE, 0x2F9BD0 }, { "Терминал", G_TERM, 0x2A3038 },
        { "Блокнот", G_NOTE, 0xE0A23B }, { "Музыка", G_MUSIC, 0xE06A4C }, { "Картинки", G_IMAGE, 0x3BA776 },
        { "Задачи", G_CHART, 0x7A5BD6 }, { "Настройки", G_GEAR, 0x6E7887 }, { "Python", G_CODE, 0x3572A5 },
        { "DOOM", G_PLAY, 0xB0302A }, { "Устройства", G_SAVE, 0x5C6B7A }, { "Компилятор", G_CODE, 0x4A5568 },
    };
    for (int i = 0; i < 12; i++) {
        int cx = x + 28 + (i % 6) * 98, cy = y + 116 + (i / 6) * 100;
        if (i == 1) ui_round_fill(cx - 6, cy - 6, 92, 92, 18, 0xFFFFFF, 160);
        app_icon(cx + 18, cy + 4, 44, pins[i].c, pins[i].g);
        int tw = ui_text_w(pins[i].n, UI_F12);
        ui_text(cx + 40 - tw / 2, cy + 56, pins[i].n, UI_F12, INK);
    }

    ui_text(x + 28, y + 330, "Недавнее", UI_F13B, INK);
    static const struct { const char *n, *m; int g; uint32_t c; } rec[] = {
        { "Отчёт за сентябрь.txt", "Блокнот · сегодня, 14:02", G_DOC, 0x4C7BD9 },
        { "github.com/prunik-96", "Браузер · сегодня, 12:40", G_GLOBE, 0x2F9BD0 },
        { "demo.mp3", "Музыка · вчера", G_MUSIC, 0xE06A4C },
    };
    for (int i = 0; i < 3; i++) {
        int ry = y + 360 + i * 50;
        if (i == 0) ui_round_fill(x + 20, ry - 4, w - 40, 46, 12, 0xFFFFFF, 120);
        app_icon(x + 32, ry + 3, 32, rec[i].c, rec[i].g);
        text_c(x + 78, ry + 12, rec[i].n, UI_F13, INK);
        text_c(x + 78, ry + 30, rec[i].m, UI_F11, DIM);
    }

    /* Foot: you, and power. */
    ui_blend(x + 20, y + h - 64, w - 40, 1, 0x000000, 25);
    rast_path p;
    rast_path_init(&p);
    rast_ellipse(&p, FXI(x + 46), FXI(y + h - 33), FXI(16), FXI(16));
    vgradp(&p, y + h - 49, y + h - 17, 0xFF000000 | mixc(ACC, 0xFFFFFF, 60), 0xFF000000 | ACC);
    rast_path_free(&p);
    text_c(x + 41, y + h - 33, "П", UI_F13B, 0xFFFFFF);
    text_c(x + 72, y + h - 33, "Пользователь", UI_F13, INK);
    ui_round_fill(x + w - 100, y + h - 52, 36, 36, 18, 0xFFFFFF, 160);
    glyph(G_GEAR, x + w - 92, y + h - 44, 20, 0xFF000000 | INK);
    ui_round_fill(x + w - 56, y + h - 52, 36, 36, 18, 0xFFFFFF, 160);
    glyph(G_POWER, x + w - 48, y + h - 44, 20, 0xFF000000 | INK);
}

/* The control panel: rises from the clock. */
static void control_panel(void) {
    int w = 380, h = 600, x = (W + 774) / 2 - w, y = H - ISL - 14 - 14 - h;
    win_shadow(x, y, w, h, 24, 1);
    ui_glass_live(x, y, w, h, 24, mixc(0xFFFFFF, ACC, 40), 175, 26);

    static const struct { const char *n, *s; int g, on; } tg[] = {
        { "Сеть", "Подключено", G_WIFI, 1 }, { "Не беспокоить", "Выкл.", G_BELL, 0 },
        { "Тёмная тема", "Выкл.", G_MOON, 0 }, { "Раскладка", "Сохранить стол", G_SAVE, 0 },
    };
    for (int i = 0; i < 4; i++) {
        int tx = x + 20 + (i % 2) * 174, ty = y + 20 + (i / 2) * 72;
        ui_round_fill(tx, ty, 166, 62, 18, tg[i].on ? ACC : 0xFFFFFF, tg[i].on ? 255 : 170);
        ui_round_fill(tx + 10, ty + 13, 36, 36, 18, tg[i].on ? 0xFFFFFF : mixc(0xFFFFFF, ACC, 40),
                      tg[i].on ? 70 : 255);
        GINNER = 0xFF000000 | (tg[i].on ? mixc(ACC, 0xFFFFFF, 70) : mixc(0xFFFFFF, ACC, 40));
        glyph(tg[i].g, tx + 18, ty + 21, 20, tg[i].on ? 0xFFFFFFFF : (0xFF000000 | mixc(ACC, 0, 40)));
        GINNER = 0xFFFFFFFF;
        ui_text(tx + 54, ty + 12, tg[i].n, UI_F13B, tg[i].on ? 0xFFFFFF : INK);
        ui_text(tx + 54, ty + 32, tg[i].s, UI_F11, tg[i].on ? 0xEEF4FF : DIM);
    }

    /* Volume. */
    int vy = y + 182;
    glyph(G_VOL, x + 26, vy + 6, 20, 0xFF000000 | INK);
    ui_round_fill(x + 60, vy + 10, w - 84, 12, 6, 0xFFFFFF, 170);
    ui_round_fill(x + 60, vy + 10, (w - 84) * 64 / 100, 12, 6, ACC, 255);
    ui_shadow(x + 60 + (w - 84) * 64 / 100 - 11, vy + 5, 22, 22, 11, 6, 50, 2, 0);
    ui_round_fill(x + 60 + (w - 84) * 64 / 100 - 11, vy + 5, 22, 22, 11, 0xFFFFFF, 255);

    /* Now playing. */
    int py = y + 230;
    ui_round_fill(x + 20, py, w - 40, 86, 18, 0xFFFFFF, 170);
    ui_round_grad(x + 32, py + 12, 62, 62, 14, UI_ALL, 0xF4A38C, 255, 0xC0508F, 255);
    glyph(G_MUSIC, x + 49, py + 29, 28, 0xE0FFFFFF);
    ui_text(x + 108, py + 16, "demo.mp3", UI_F15B, INK);
    ui_text(x + 108, py + 38, "Музыка · 0:12 / 0:21", UI_F12, DIM);
    ui_round_fill(x + 108, py + 62, 150, 4, 2, 0xD5DAE0, 255);
    ui_round_fill(x + 108, py + 62, 86, 4, 2, ACC, 255);
    glyph(G_PREV, x + 276, py + 30, 18, 0xFF000000 | INK);
    glyph(G_PLAY, x + 304, py + 28, 22, 0xFF000000 | INK);
    glyph(G_NEXT, x + 334, py + 30, 18, 0xFF000000 | INK);

    /* The month. */
    int cy = y + 332;
    ui_round_fill(x + 20, cy, w - 40, h - 352, 18, 0xFFFFFF, 170);
    ui_text(x + 36, cy + 14, "Октябрь 2026", UI_F15B, INK);
    static const char *wd[7] = { "Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс" };
    for (int i = 0; i < 7; i++) {
        int tw = ui_text_w(wd[i], UI_F11);
        ui_text(x + 36 + i * 44 + 14 - tw / 2, cy + 46, wd[i], UI_F11, DIM);
    }
    int day = 1, col = 3;                     /* 1 October 2026 is a Thursday */
    for (int row = 0; row < 5 && day <= 31; row++) {
        for (; col < 7 && day <= 31; col++, day++) {
            char b[3] = { (char)(day >= 10 ? '0' + day / 10 : '0' + day), (char)(day >= 10 ? '0' + day % 10 : 0), 0 };
            int dx = x + 36 + col * 44 + 14, dy = cy + 74 + row * 30;
            if (day == 1) ui_round_fill(dx - 14, dy - 4, 28, 26, 13, ACC, 255);
            int tw = ui_text_w(b, UI_F12);
            ui_text(dx - tw / 2, dy, b, UI_F12, day == 1 ? 0xFFFFFF : (col >= 5 ? DIM : INK));
        }
        col = 0;
    }
}

static void toast(void) {
    int w = 380, h = 86, x = W - w - 24, y = 24;
    win_shadow(x, y, w, h, 20, 1);
    ui_glass_live(x, y, w, h, 20, mixc(0xFFFFFF, ACC, 40), 175, 24);
    app_icon(x + 18, y + 20, 44, 0xE06A4C, G_MUSIC);
    ui_text(x + 76, y + 18, "Сейчас играет", UI_F13B, INK);
    ui_text(x + 76, y + 40, "demo.mp3 — 21 секунда", UI_F13, DIM);
    glyph(G_CLOSE, x + w - 30, y + 14, 12, 0xFF000000 | DIM);
}

/* A small window, pinned above everything and half see-through. */
static void mini_player(int x, int y) {
    int w = 340, h = 120;
    win_shadow(x, y, w, h, 20, 0);
    ui_glass_live(x, y, w, h, 20, mixc(0xFFFFFF, ACC, 40), 120, 24);
    ui_round_grad(x + 16, y + 16, 88, 88, 16, UI_ALL, 0xF4A38C, 255, 0xC0508F, 255);
    glyph(G_MUSIC, x + 38, y + 38, 44, 0xE0FFFFFF);
    ui_text(x + 120, y + 18, "demo.mp3", UI_F15B, INK);
    ui_text(x + 120, y + 40, "0:12 / 0:21", UI_F12, DIM);
    glyph(G_PREV, x + 124, y + 74, 18, 0xFF000000 | INK);
    ui_round_fill(x + 154, y + 66, 36, 36, 18, ACC, 255);
    glyph(G_PLAY, x + 164, y + 76, 16, 0xFFFFFFFF);
    glyph(G_NEXT, x + 202, y + 74, 18, 0xFF000000 | INK);
    ui_round_fill(x + w - 40, y + 10, 28, 28, 14, ACC, 255);
    glyph(G_PIN, x + w - 33, y + 17, 14, 0xFFFFFFFF);
}

static void pointer(int x, int y) {
    int hx, hy;
    const uint32_t *img = cursor_image(CUR_ARROW, 0, &hx, &hy);
    ui_image(x - hx, y - hy, img, CUR_SZ, CUR_SZ, 255);
}


/* --- marks for the logo ---------------------------------------------------- */

/* The mark, white, centred on (cx, cy), about 2R across. */
static void logo_mark(int kind, int cx, int cy, int R) {
    rast_path p;
    rast_path_init(&p);
    rast_fx X = FXI(cx), Y = FXI(cy), RR = FXI(R);
#define K(v) (rast_fx)((int64_t)RR * (v) / 100)
    switch (kind) {
    case 0:                                   /* an orbit: a ring and its moon */
        rast_ellipse(&p, X, Y + K(4), K(44), K(44));
        strokep(&p, K(17), 0xFFFFFFFF);
        rast_path_reset(&p);
        rast_ellipse(&p, X + K(34), Y - K(30), K(17), K(17));
        fillp(&p, 0xFFFFFFFF);
        break;
    case 1:                                   /* pebbles: three, worn smooth */
        rast_ellipse(&p, X - K(16), Y + K(12), K(38), K(38));
        fillp(&p, 0xFFFFFFFF);
        rast_path_reset(&p);
        rast_ellipse(&p, X + K(30), Y - K(22), K(24), K(24));
        fillp(&p, 0xD0FFFFFF);
        rast_path_reset(&p);
        rast_ellipse(&p, X + K(36), Y + K(36), K(14), K(14));
        fillp(&p, 0xA0FFFFFF);
        break;
    case 2:                                   /* a window that is a tab */
        rast_round_rect(&p, X - K(52), Y - K(30), K(104), K(80), K(22), K(22));
        rast_round_rect(&p, X - K(52), Y - K(50), K(56), K(40), K(18), K(18));
        fillp(&p, 0xFFFFFFFF);
        rast_path_reset(&p);
        rast_round_rect(&p, X - K(30), Y - K(6), K(60), K(10), K(5), K(5));
        rast_round_rect(&p, X - K(30), Y + K(14), K(40), K(10), K(5), K(5));
        fillp(&p, GINNER);
        break;
    default:                                  /* an x that flows */
        rast_move_to(&p, X - K(44), Y - K(44));
        rast_cubic_to(&p, X - K(4), Y - K(30), X + K(4), Y + K(30), X + K(44), Y + K(44));
        rast_move_to(&p, X + K(44), Y - K(44));
        rast_cubic_to(&p, X + K(10), Y - K(10), X - K(10), Y + K(10), X - K(44), Y + K(44));
        strokep(&p, K(20), 0xFFFFFFFF);
        break;
    }
#undef K
    rast_path_free(&p);
}

static void logo_disc(int kind, int cx, int cy, int R, uint32_t col) {
    rast_path p;
    rast_path_init(&p);
    rast_ellipse(&p, FXI(cx), FXI(cy), FXI(R), FXI(R));
    vgradp(&p, cy - R, cy + R, 0xFF000000 | mixc(col, 0xFFFFFF, 70), 0xFF000000 | mixc(col, 0, 30));
    rast_path_free(&p);
    GINNER = 0xFF000000 | mixc(col, 0, 10);
    logo_mark(kind, cx, cy, R * 88 / 100);
    GINNER = 0xFFFFFFFF;
}

static void logo_sheet(void) {
    wall_soft(cv, W, H, WALL_DAWN);
    ui_target(cv, W, H);
    for (int i = 0; i < W * H; i++) cv[i] = ui_mix(cv[i], 0xFFFFFF, 150);
    ACC = 0xD2695A;
    static const char *names[4] = { "1. Орбита", "2. Галька", "3. Окно-вкладка", "4. Течение" };
    static const char *why[4] = { "кольцо и спутник: система,", "три гладких камня: спокойно",
                                  "окно, у которого верх —", "буква x одной плавной" };
    static const char *why2[4] = { "вокруг которой всё крутится", "и мягко, как сам стиль",
                                   "вкладка, как в новом стиле", "линией, без углов" };
    static const uint32_t cols[3] = { 0xD2695A, 0x34B6D2, 0x5A78D2 };
    ui_text(120, 70, "Знак xyuOS — четыре варианта", UI_F28, INK);
    ui_text(122, 118, "В цвете каждых из трёх обоев, крупно и на кнопке острова (44 px).", UI_F15, DIM);
    for (int k = 0; k < 4; k++) {
        int cx = 120 + k * 430 + 170;
        ui_round_fill(cx - 190, 180, 380, 820, 28, 0xFFFFFF, 170);
        logo_disc(k, cx, 360, 130, cols[0]);
        for (int c = 0; c < 3; c++) logo_disc(k, cx - 100 + c * 100, 600, 34, cols[c]);
        /* on an island chip */
        ui_round_fill(cx - 120, 690, 240, 64, 26, mixc(0xFFFFFF, cols[0], 60), 200);
        logo_disc(k, cx - 120 + 32, 722, 22, cols[0]);
        app_icon(cx - 120 + 66, 700, 44, 0x3E7BE0, G_FOLDER);
        app_icon(cx - 120 + 118, 700, 44, 0x2F9BD0, G_GLOBE);
        app_icon(cx - 120 + 170, 700, 44, 0x2A3038, G_TERM);
        int tw = ui_text_w(names[k], UI_F20B);
        ui_text(cx - tw / 2, 800, names[k], UI_F20B, INK);
        tw = ui_text_w(why[k], UI_F13);
        ui_text(cx - tw / 2, 846, why[k], UI_F13, DIM);
        tw = ui_text_w(why2[k], UI_F13);
        ui_text(cx - tw / 2, 868, why2[k], UI_F13, DIM);
    }
}

/* --- the three variants ------------------------------------------------------ */

static void scene(int v) {
    int style = v == 0 ? WALL_DAWN : v == 1 ? WALL_LAGOON : WALL_MIST;
    wall_soft(cv, W, H, style);
    ACC = ui_accent_from(cv, W, H);
    printf("variant %d: accent %06X\n", v, ACC);
    ui_target(cv, W, H);

    int cx, cy, cw, ch;
    if (v == 0) {
        tab_t t1[] = { { "Терминал", G_TERM, 0x2A3038 } };
        window(380, 90, 760, 470, t1, 1, 0, 0, -1, 0, &cx, &cy, &cw, &ch);
        term_body(cx, cy, cw, ch);
        tab_t t2[] = { { "Документы", G_FOLDER, 0x3E7BE0 }, { "Картинки", G_IMAGE, 0x3BA776 } };
        window(820, 190, 900, 560, t2, 2, 0, 1, 3, 0, &cx, &cy, &cw, &ch);
        files_body(cx, cy, cw, ch);
        island(0, 1, 0);
        launcher();
        toast();
        pointer(842, 260);
    } else if (v == 1) {
        tab_t t1[] = { { "Новости xyuOS", G_GLOBE, 0x2F9BD0 }, { "GitHub", G_CODE, 0x2A3038 },
                       { "Погода", G_GLOBE, 0x2F9BD0 } };
        window(170, 70, 980, 640, t1, 3, 0, 1, -1, 0, &cx, &cy, &cw, &ch);
        browser_body(cx, cy, cw, ch);
        tab_t t2[] = { { "Список дел", G_NOTE, 0xE0A23B } };
        window(1060, 120, 560, 380, t2, 1, 0, 0, -1, 0, &cx, &cy, &cw, &ch);
        notes_body(cx, cy, cw, ch);
        island(0, 0, 1);
        control_panel();
        pointer(1300, 1040);
    } else {
        tab_t t2[] = { { "Документы", G_FOLDER, 0x3E7BE0 } };
        window(260, 130, 1000, 640, t2, 1, 0, 1, 1, 0, &cx, &cy, &cw, &ch);
        files_body(cx, cy, cw, ch);
        tab_t t1[] = { { "Терминал", G_TERM, 0x2A3038 } };
        window(1140, 420, 640, 420, t1, 1, 0, 0, -1, 0, &cx, &cy, &cw, &ch);
        term_body(cx, cy, cw, ch);
        mini_player(1520, 60);
        island(1, 0, 0);
        pointer(1260, 160);
    }
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "/tmp/mock";
    if (!uifont_init()) { fprintf(stderr, "no fonts\n"); return 1; }
    {   /* The terminal's face, for the pictures. */
        uint8_t *d = load_file("/fonts/NotoSansMono-Regular.ttf");
        stbtt_fontinfo fi;
        if (d && stbtt_InitFont(&fi, d, 0)) build_face(&mono13, &fi, 13);
    }
    cursor_init();
    cv = malloc((size_t)W * H * 4);
    for (int v = 0; v < 3; v++) {
        scene(v);
        char path[256];
        snprintf(path, sizeof path, "%s%d", out, v);
        save_png(path);
    }
    logo_sheet();
    {
        char path[256];
        snprintf(path, sizeof path, "%s3", out);
        save_png(path);
    }
    return 0;
}
