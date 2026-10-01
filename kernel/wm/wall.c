// The default wallpaper, and the frosted copy of it that glass shows.
//
// In the spirit of Windows 7's: a deep blue sky, brightest a little above the
// middle, crossed by sweeping strokes of light -- thin, tapering to nothing at
// both ends, cyan and white where they cross the bright middle, warming to
// green, gold and orange toward the lower right. Not a picture of anything --
// no logo -- and made here, at whatever size the screen is, so it is never
// scaled and never blurry.
//
// Drawn with librast (integer only) and softened with the box blur in ui.c.
// Each stroke is laid down twice: once as itself, and once onto black, spread
// wide and added back with a screen blend -- the glow around light, which
// brightens what is under it and never darkens it.

#include "wall.h"
#include "ui.h"
#include "../mm/heap.h"
#include "../../librast/include/rast.h"

// Per-mille of the screen to 16.16 pixels.
static int W_, H_;
static rast_fx PX(int xm) { return (rast_fx)((int64_t)xm * W_ * RAST_ONE / 1000); }
static rast_fx PY(int ym) { return (rast_fx)((int64_t)ym * H_ * RAST_ONE / 1000); }

static void target(rast_target *t, uint32_t *px) {
    rast_target_init(t, px, W_, H_, W_, RAST_XRGB);
}

static void zero_paint(rast_paint *p) {
    for (unsigned i = 0; i < sizeof *p; i++) ((volatile uint8_t *)p)[i] = 0;
    p->m = rast_identity();
    p->spread = RAST_PAD;
    p->opacity = 255;
}

static void fill_all(uint32_t *px, rast_paint *p) {
    rast_target t;
    target(&t, px);
    rast_path path;
    rast_path_init(&path);
    rast_rect(&path, 0, 0, RAST_INT(W_), RAST_INT(H_));
    rast_fill(&t, &path, 0, RAST_NONZERO, p);
    rast_path_free(&path);
}

static void radial(rast_paint *p, int cxm, int cym, int rm, const rast_stop *st, int n) {
    zero_paint(p);
    p->type = RAST_RADIAL;
    p->cx = PX(cxm); p->cy = PY(cym);
    p->fx = p->cx;   p->fy = p->cy;
    p->r = (rast_fx)((int64_t)rm * (W_ > H_ ? W_ : H_) * RAST_ONE / 1000);
    p->stops = st; p->nstops = n;
}

#define F(v) ((rast_fx)((int64_t)(v) * RAST_ONE / 1000))   // 0..1000 -> 0..1

// A stroke of light: a lens between two cubics that share their ends, so it
// tapers to a point at both. The curve is given once; `thick` (per mille of
// the height) pushes the control points apart for the upper and lower edge.
struct swoosh {
    int p[8];            // x0 y0  c1x c1y  c2x c2y  x1 y1, per mille
    int thick;
    rast_stop stops[5];
    int nstops;
};

static void draw_swoosh(uint32_t *layer, const struct swoosh *s, int opacity) {
    const int *a = s->p;
    int t = s->thick;
    rast_path p;
    rast_path_init(&p);
    rast_move_to(&p, PX(a[0]), PY(a[1]));
    rast_cubic_to(&p, PX(a[2]), PY(a[3] - t), PX(a[4]), PY(a[5] - t), PX(a[6]), PY(a[7]));
    rast_cubic_to(&p, PX(a[4]), PY(a[5] + t), PX(a[2]), PY(a[3] + t), PX(a[0]), PY(a[1]));
    rast_close(&p);
    rast_paint paint;
    zero_paint(&paint);
    paint.type = RAST_LINEAR;
    paint.x1 = PX(a[0]); paint.x2 = PX(a[6]);
    paint.stops = s->stops; paint.nstops = s->nstops;
    paint.opacity = opacity;
    rast_target tg;
    target(&tg, layer);
    rast_fill(&tg, &p, 0, RAST_NONZERO, &paint);
    rast_path_free(&p);
}

static inline uint32_t screen(uint32_t a, uint32_t b, int k /* 0..256 of b */) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t x = (a >> sh) & 255, y = ((b >> sh) & 255) * (uint32_t)k >> 8;
        out |= (x + y - (x * y + 127) / 255) << sh;
    }
    return out;
}

// --- the two skies -------------------------------------------------------------

static const rast_stop sky_light[] = {
    { F(0),    0xFF63C6FF }, { F(200), 0xFF2F95EA }, { F(560), 0xFF0E5BC0 },
    { F(1000), 0xFF06306E },
};
static const rast_stop sky_dark[] = {
    { F(0),    0xFF1E4A84 }, { F(220), 0xFF123262 }, { F(580), 0xFF0A1834 },
    { F(1000), 0xFF03060D },
};

static const struct swoosh sw_light[] = {
    // The long one: up from the lower left through the bright middle.
    { { -80, 900,  300, 770,  620, 330, 1080, 250 }, 46,
      { { F(0), 0x0080E8FF }, { F(300), 0x9070DCFF }, { F(480), 0xE0F4FCFF },
        { F(720), 0xA0B8F070 }, { F(1000), 0x30F0D040 } }, 5 },
    // Green into gold into orange, along the bottom.
    { { 40, 1090,  380, 830,  700, 620, 1090, 540 }, 34,
      { { F(0), 0x0070D040 }, { F(320), 0xB088D848 }, { F(600), 0xC8F4D848 },
        { F(860), 0xA0F8A030 }, { F(1000), 0x40F07020 } }, 5 },
    // A slimmer cyan one above.
    { { -80, 640,  260, 590,  560, 210, 1080, 90 }, 22,
      { { F(0), 0x0090D8FF }, { F(380), 0x9098DCFF }, { F(640), 0x80D0F4FF },
        { F(1000), 0x10FFFFFF } }, 4 },
    // A warm wisp low on the right.
    { { 420, 1090,  640, 910,  820, 800, 1090, 770 }, 16,
      { { F(0), 0x00FFB050 }, { F(500), 0xA0FFA050 }, { F(1000), 0x60FF7088 } }, 3 },
    // A hair-thin white one crossing the big one.
    { { -80, 820,  320, 700,  600, 380, 1080, 330 }, 5,
      { { F(0), 0x00FFFFFF }, { F(450), 0xF0FFFFFF }, { F(1000), 0x40FFFFFF } }, 3 },
};
#define N_SW_LIGHT (int)(sizeof sw_light / sizeof sw_light[0])

static const struct swoosh sw_dark[] = {
    { { -80, 900,  300, 770,  620, 330, 1080, 250 }, 46,
      { { F(0), 0x003C8CF0 }, { F(320), 0x704A9CF8 }, { F(500), 0xA088C8FF },
        { F(760), 0x705A60E8 }, { F(1000), 0x208040D0 } }, 5 },
    { { 40, 1090,  380, 830,  700, 620, 1090, 540 }, 34,
      { { F(0), 0x0020B0B8 }, { F(340), 0x8028B4C8 }, { F(640), 0x904C78F0 },
        { F(1000), 0x309050E0 } }, 4 },
    { { -80, 640,  260, 590,  560, 210, 1080, 90 }, 22,
      { { F(0), 0x003060C8 }, { F(420), 0x603868D8 }, { F(1000), 0x003868D8 } }, 3 },
    { { -80, 820,  320, 700,  600, 380, 1080, 330 }, 5,
      { { F(0), 0x0090C8FF }, { F(450), 0xA0A0D4FF }, { F(1000), 0x2090C8FF } }, 3 },
};
#define N_SW_DARK (int)(sizeof sw_dark / sizeof sw_dark[0])

void wall_paint(uint32_t *px, int w, int h, int dark) {
    W_ = w; H_ = h;
    int scale = (w > h ? w : h);

    rast_paint p;
    radial(&p, 500, 430, 760, dark ? sky_dark : sky_light, 4);
    fill_all(px, &p);

    const struct swoosh *sw = dark ? sw_dark : sw_light;
    int nsw = dark ? N_SW_DARK : N_SW_LIGHT;

    uint32_t *glow = (uint32_t *)kmalloc((size_t)w * h * 4);
    if (glow) {
        // The glow: the strokes onto black, spread wide, added to the sky.
        for (int i = 0; i < w * h; i++) glow[i] = 0;
        for (int i = 0; i < nsw; i++) draw_swoosh(glow, &sw[i], 255);
        ui_blur(glow, w, h, scale / 60 + 2);
        for (int i = 0; i < w * h; i++) px[i] = screen(px[i], glow[i], dark ? 200 : 170);
    }

    // The strokes themselves, laid straight onto the sky so they keep their
    // colour -- light screened onto a bright sky only ever turns white.
    for (int i = 0; i < nsw; i++) draw_swoosh(px, &sw[i], 255);
    ui_blur(px, w, h, scale / 1100 + 1);         // and their edges softened

    // A burst of light where the strokes cross.
    static const rast_stop flare_light[] = {
        { F(0), 0x90FFFFFF }, { F(220), 0x40DCF4FF }, { F(1000), 0x00FFFFFF } };
    static const rast_stop flare_dark[] = {
        { F(0), 0x6080B8FF }, { F(260), 0x205080D0 }, { F(1000), 0x000000FF } };
    radial(&p, 470, 470, 230, dark ? flare_dark : flare_light, 3);
    fill_all(px, &p);

    // Out-of-focus dots of light, from a fixed sequence so the wallpaper is
    // the same on every boot.
    if (glow) {
        uint32_t seed = 0x9E3779B9u;
        for (int i = 0; i < w * h; i++) glow[i] = 0;
        for (int k = 0; k < 22; k++) {
            seed = seed * 1103515245u + 12345u;
            int xm = (int)((seed >> 8) % 1000);
            seed = seed * 1103515245u + 12345u;
            int ym = 100 + (int)((seed >> 8) % 800);
            seed = seed * 1103515245u + 12345u;
            int rpx = scale / 220 + (int)((seed >> 8) % (uint32_t)(scale / 70 + 1));
            seed = seed * 1103515245u + 12345u;
            int a = 0x14 + (int)((seed >> 8) % 0x28);
            if (dark) a = a * 2 / 3;
            rast_path c;
            rast_path_init(&c);
            rast_ellipse(&c, PX(xm), PY(ym), RAST_INT(rpx), RAST_INT(rpx));
            rast_paint dot;
            rast_paint_solid(&dot, ((uint32_t)a << 24) | (dark ? 0x9CC8FF : 0xE8F6FF));
            rast_target t;
            target(&t, glow);
            rast_fill(&t, &c, 0, RAST_NONZERO, &dot);
            rast_path_free(&c);
        }
        ui_blur(glow, w, h, scale / 480 + 1);
        for (int i = 0; i < w * h; i++) px[i] = screen(px[i], glow[i], 256);
        kfree(glow);
    }

    // Darker toward the corners, so the eye rests in the middle.
    static const rast_stop vig[] = {
        { F(0), 0x00000000 }, { F(560), 0x00000000 }, { F(1000), 0x78000818 } };
    radial(&p, 500, 450, 760, vig, 3);
    fill_all(px, &p);
}

void wall_frost(const uint32_t *src, uint32_t *dst, int w, int h, int dark) {
    for (int i = 0; i < w * h; i++) dst[i] = src[i];
    int scale = (w > h ? w : h);
    ui_blur(dst, w, h, scale / 90 + 2);
    // Glass seen through makes colour richer, not greyer: lift the
    // saturation, and the brightness a little in the light theme.
    for (int i = 0; i < w * h; i++) {
        uint32_t c = dst[i];
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        int y = (r * 77 + g * 150 + b * 29) >> 8;
        int s = 330;                                   // 1.3x
        r = y + (r - y) * s / 256;
        g = y + (g - y) * s / 256;
        b = y + (b - y) * s / 256;
        if (!dark) { r += (255 - r) / 8; g += (255 - g) / 8; b += (255 - b) / 8; }
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
        dst[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
