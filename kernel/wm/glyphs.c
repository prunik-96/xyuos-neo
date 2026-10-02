// The desktop's small pictures (see glyphs.h).

#include "glyphs.h"
#include "ui.h"

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
    for (unsigned i = 0; i < sizeof pt; i++) ((volatile uint8_t *)&pt)[i] = 0;
    pt.type = RAST_LINEAR;
    pt.m = rast_identity();
    pt.x1 = 0; pt.y1 = RAST_INT(y0); pt.x2 = 0; pt.y2 = RAST_INT(y1);
    pt.stops = st; pt.nstops = 2; pt.opacity = 255; pt.spread = RAST_PAD;
    ui_rast_fill(p, &pt);
}

void glyph_draw(int g, int x, int y, int s, uint32_t c, uint32_t inner) {
    rast_path p;
    rast_path_init(&p);
    rast_fx X = RAST_INT(x), Y = RAST_INT(y), S = RAST_INT(s);
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
        strokep(&p, LEN(8), inner);
        break;
    case G_NOTE: case G_DOC:
        rast_round_rect(&p, PT(16, 6), LEN(68), LEN(88), LEN(12), LEN(12));
        fillp(&p, c);
        rast_path_reset(&p);
        for (int k = 0; k < 4; k++) {
            rast_move_to(&p, PT(30, 30 + k * 14));
            rast_line_to(&p, PT(k == 3 ? 52 : 70, 30 + k * 14));
        }
        strokep(&p, LEN(6), inner);
        break;
    case G_PLAY:
        rast_move_to(&p, PT(30, 18)); rast_line_to(&p, PT(82, 50)); rast_line_to(&p, PT(30, 82));
        rast_close(&p);
        fillp(&p, c);
        strokep(&p, LEN(10), c);
        break;
    case G_PAUSE:
        rast_round_rect(&p, PT(24, 18), LEN(18), LEN(64), LEN(6), LEN(6));
        rast_round_rect(&p, PT(58, 18), LEN(18), LEN(64), LEN(6), LEN(6));
        fillp(&p, c);
        break;
    case G_MUSIC:
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
        fillp(&p, inner);
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
        fillp(&p, inner);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(70, 34), LEN(8), LEN(8));
        fillp(&p, inner);
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
        fillp(&p, inner);
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
        fillp(&p, inner);
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
    case G_GAME:
        rast_round_rect(&p, PT(6, 30), LEN(88), LEN(44), LEN(22), LEN(22));
        fillp(&p, c);
        rast_path_reset(&p);
        rast_move_to(&p, PT(22, 52)); rast_line_to(&p, PT(38, 52));
        rast_move_to(&p, PT(30, 44)); rast_line_to(&p, PT(30, 60));
        strokep(&p, LEN(6), inner);
        rast_path_reset(&p);
        rast_ellipse(&p, PT(66, 48), LEN(5), LEN(5));
        rast_ellipse(&p, PT(76, 58), LEN(5), LEN(5));
        fillp(&p, inner);
        break;
    case G_CHIP:
        rast_round_rect(&p, PT(22, 22), LEN(56), LEN(56), LEN(10), LEN(10));
        fillp(&p, c);
        rast_path_reset(&p);
        for (int k = 0; k < 3; k++) {
            int o = 34 + k * 16;
            rast_move_to(&p, PT(o, 8));  rast_line_to(&p, PT(o, 20));
            rast_move_to(&p, PT(o, 80)); rast_line_to(&p, PT(o, 92));
            rast_move_to(&p, PT(8, o));  rast_line_to(&p, PT(20, o));
            rast_move_to(&p, PT(80, o)); rast_line_to(&p, PT(92, o));
        }
        strokep(&p, LEN(6), c);
        break;
    }
#undef PT
#undef LEN
    rast_path_free(&p);
}

void glyph_app(int x, int y, int s, uint32_t col, int g) {
    rast_path p;
    rast_path_init(&p);
    int r = s * 28 / 100;
    rast_round_rect(&p, RAST_INT(x), RAST_INT(y), RAST_INT(s), RAST_INT(s), RAST_INT(r), RAST_INT(r));
    vgradp(&p, y, y + s, 0xFF000000 | ui_mix(col, 0xFFFFFF, 50), 0xFF000000 | ui_mix(col, 0, 20));
    rast_path_free(&p);
    ui_rrect_line(x, y, s, s, r, UI_ALL, 0x00FFFFFF, 70);
    int gs = s * 56 / 100;
    glyph_draw(g, x + (s - gs) / 2, y + (s - gs) / 2, gs, 0xFFFFFFFF, 0xFF000000 | ui_mix(col, 0, 10));
}

void glyph_logo(int cx, int cy, int R, uint32_t col) {
    rast_path p;
    rast_path_init(&p);
    rast_ellipse(&p, RAST_INT(cx), RAST_INT(cy), RAST_INT(R), RAST_INT(R));
    vgradp(&p, cy - R, cy + R, 0xFF000000 | ui_mix(col, 0xFFFFFF, 70), 0xFF000000 | ui_mix(col, 0, 30));
    rast_path_reset(&p);
    // An x drawn in one flowing line: two strokes that each bend as they
    // cross, round at the ends.
    rast_fx X = RAST_INT(cx), Y = RAST_INT(cy);
    rast_fx K = RAST_INT(R) * 54 / 100;          // the mark's half-size
#define K_(v) (rast_fx)((int64_t)K * (v) / 100)
    rast_move_to(&p, X - K_(80), Y - K_(80));
    rast_cubic_to(&p, X - K_(8), Y - K_(54), X + K_(8), Y + K_(54), X + K_(80), Y + K_(80));
    rast_move_to(&p, X + K_(80), Y - K_(80));
    rast_cubic_to(&p, X + K_(18), Y - K_(18), X - K_(18), Y + K_(18), X - K_(80), Y + K_(80));
    strokep(&p, K_(36), 0xFFFFFFFF);
#undef K_
    rast_path_free(&p);
}

void glyph_for_program(const char *name, int *g, uint32_t *col) {
    static const struct { const char *n; int g; uint32_t c; } map[] = {
        { "files",   G_FOLDER, 0x003E7BE0 }, { "fm",      G_FOLDER, 0x003E7BE0 },
        { "web",     G_GLOBE,  0x002F9BD0 }, { "netsurf", G_GLOBE,  0x002F7FB8 },
        { "sh",      G_TERM,   0x002A3038 }, { "note",    G_NOTE,   0x00E0A23B },
        { "edit",    G_NOTE,   0x00D08A2B }, { "play",    G_MUSIC,  0x00E06A4C },
        { "view",    G_IMAGE,  0x003BA776 }, { "taskmgr", G_CHART,  0x007A5BD6 },
        { "control", G_GEAR,   0x006E7887 }, { "devmgr",  G_CHIP,   0x005C6B7A },
        { "python",  G_CODE,   0x003572A5 }, { "cc",      G_CODE,   0x004A5568 },
        { "doom",    G_GAME,   0x00B0302A }, { "plasma",  G_IMAGE,  0x00A04BC8 },
        { "netlog",  G_WIFI,   0x002E8B6E },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        const char *a = map[i].n, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) { *g = map[i].g; *col = map[i].c; return; }
    }
    *g = G_TERM;
    *col = 0x00586474;
}
