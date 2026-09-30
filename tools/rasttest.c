/* librast, built for the host and checked against geometry.
 *
 *   gcc -O2 -Wall -fsanitize=address,undefined -Ilibrast/include -Ilibrast/src \
 *       librast/src/[a-z]*.c tools/rasttest.c -lm -o /tmp/rasttest && /tmp/rasttest /tmp/rast
 *
 * Coverage is measured by drawing opaque black into a transparent premultiplied
 * buffer: every pixel's alpha is then exactly how much of it the shape covers.
 * Areas are checked against the formulas, single pixels against what their
 * edges say they must be, the two fill rules against shapes whose answer is
 * known, strokes against the widths and lengths they were given. Random
 * shapes with absurd coordinates run under the sanitizers to catch anything
 * that reads, writes or overflows where it should not. And a test sheet is
 * written as a picture, for the eye.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "rast.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define W 400
#define H 300
static uint32_t buf[W * H];

static rast_fx F(double v) { return (rast_fx)lrint(v * 65536.0); }

static void clear(void) { memset(buf, 0, sizeof buf); }
static int cov(int x, int y) { return (int)(buf[y * W + x] >> 24); }
static double area(void) {
    double s = 0;
    for (int i = 0; i < W * H; i++) s += (buf[i] >> 24) / 255.0;
    return s;
}

static rast_target T;
static rast_paint black;

static void fill(rast_path *p, int rule) { rast_fill(&T, p, NULL, rule, &black); }

static void near(double got, double want, double rel, const char *what) {
    double err = fabs(got - want) / (want > 1 ? want : 1);
    CHECK(err <= rel, "%s: %.3f, expected %.3f (%.3f%% off)", what, got, want, err * 100);
    if (err <= rel) printf("ok: %s %.2f (expected %.2f)\n", what, got, want);
}

static void t_fractional_rect(void) {
    clear();
    rast_path p; rast_path_init(&p);
    rast_rect(&p, F(10.25), F(5.5), F(10.5), F(9.5));      /* 10.25..20.75, 5.5..15 */
    fill(&p, RAST_NONZERO);
    near(area(), 10.5 * 9.5, 0.002, "rectangle with fractional edges, area");
    CHECK(abs(cov(10, 10) - 191) <= 1, "left column 3/4 covered: %d", cov(10, 10));
    CHECK(abs(cov(20, 10) - 191) <= 1, "right column 3/4 covered: %d", cov(20, 10));
    CHECK(abs(cov(15, 5) - 128) <= 1, "top row half covered: %d", cov(15, 5));
    CHECK(cov(15, 10) == 255, "inside fully covered: %d", cov(15, 10));
    CHECK(cov(9, 10) == 0 && cov(21, 10) == 0 && cov(15, 15) == 0, "outside untouched");
    CHECK(abs(cov(10, 5) - 96) <= 1, "corner 3/8 covered: %d", cov(10, 5));
    rast_path_free(&p);
}

static void t_circle(void) {
    clear();
    rast_path p; rast_path_init(&p);
    rast_ellipse(&p, F(100.5), F(100.25), F(40.3), F(40.3));
    fill(&p, RAST_NONZERO);
    /* Flattened curves lie inside the curve: at a tenth of a pixel that is
     * about 0.2% of a circle this size. */
    near(area(), M_PI * 40.3 * 40.3, 0.003, "circle r=40.3, area");
    rast_path_free(&p);

    clear();
    rast_path_init(&p);
    rast_move_to(&p, F(260), F(100));                      /* a circle from two arcs */
    rast_arc_to(&p, F(50), F(50), 0, 0, 1, F(360), F(100));
    rast_arc_to(&p, F(50), F(50), 0, 0, 1, F(260), F(100));
    rast_close(&p);
    fill(&p, RAST_NONZERO);
    near(area(), M_PI * 50 * 50, 0.003, "circle from two SVG arcs, area");
    CHECK(cov(310, 100) == 255 && cov(310, 52) == 255 && cov(310, 147) == 255 &&
          cov(310, 48) == 0 && cov(310, 151) == 0 && cov(258, 100) == 0 && cov(361, 100) == 0,
          "SVG arc circle in the right place");
    rast_path_free(&p);
}

static void t_polygons(void) {
    clear();
    rast_path p; rast_path_init(&p);
    rast_move_to(&p, F(0), F(0));
    rast_line_to(&p, F(200), F(0));
    rast_line_to(&p, F(0), F(200));
    rast_close(&p);
    fill(&p, RAST_NONZERO);
    near(area(), 20000, 0.001, "right triangle, area");
    rast_path_free(&p);

    /* A 60x60 square rotated 30 degrees about its centre. */
    clear();
    rast_path_init(&p);
    rast_rect(&p, F(-30), F(-30), F(60), F(60));
    rast_mat m = rast_mul(rast_translate(F(150.3), F(120.7)), rast_rotate(F(30)));
    rast_fill(&T, &p, &m, RAST_NONZERO, &black);
    near(area(), 3600, 0.002, "rotated square, area");
    rast_path_free(&p);
}

static void t_rules(void) {
    /* Two squares, one inside the other, wound the same way. */
    rast_path p; rast_path_init(&p);
    rast_rect(&p, F(50), F(50), F(100), F(100));
    rast_rect(&p, F(75), F(75), F(50), F(50));
    clear(); fill(&p, RAST_NONZERO);
    CHECK(cov(100, 100) == 255, "nonzero: same winding fills the middle (%d)", cov(100, 100));
    near(area(), 10000, 0.001, "nonzero, nested squares, area");
    clear(); fill(&p, RAST_EVENODD);
    CHECK(cov(100, 100) == 0, "evenodd: the middle is a hole (%d)", cov(100, 100));
    near(area(), 7500, 0.001, "evenodd, nested squares, area");
    rast_path_free(&p);

    /* Wound the other way, both rules leave a hole. */
    rast_path_init(&p);
    rast_rect(&p, F(50), F(50), F(100), F(100));
    rast_move_to(&p, F(75), F(75));
    rast_line_to(&p, F(75), F(125));
    rast_line_to(&p, F(125), F(125));
    rast_line_to(&p, F(125), F(75));
    rast_close(&p);
    clear(); fill(&p, RAST_NONZERO);
    CHECK(cov(100, 100) == 0, "nonzero: opposite winding cuts a hole (%d)", cov(100, 100));
    rast_path_free(&p);

    /* A pentagram: its middle winds twice. */
    rast_path_init(&p);
    for (int i = 0; i < 5; i++) {
        double a = -M_PI / 2 + i * 4 * M_PI / 5;
        rast_fx x = F(200 + 100 * cos(a)), y = F(150 + 100 * sin(a));
        if (i == 0) rast_move_to(&p, x, y); else rast_line_to(&p, x, y);
    }
    rast_close(&p);
    clear(); fill(&p, RAST_NONZERO);
    CHECK(cov(200, 150) == 255, "pentagram nonzero: centre filled");
    clear(); fill(&p, RAST_EVENODD);
    CHECK(cov(200, 150) == 0, "pentagram evenodd: centre empty");
    CHECK(cov(200, 70) == 255, "pentagram evenodd: a point filled (%d)", cov(200, 70));
    rast_path_free(&p);
}

static void t_clip(void) {
    rast_path p; rast_path_init(&p);
    rast_ellipse(&p, F(200), F(150), F(120), F(100));
    clear(); fill(&p, RAST_NONZERO);
    static uint32_t whole[W * H];
    memcpy(whole, buf, sizeof buf);

    clear();
    rast_target keep = T;
    T.cx0 = 150; T.cy0 = 100; T.cx1 = 260; T.cy1 = 180;
    fill(&p, RAST_NONZERO);
    T = keep;
    int bad_in = 0, bad_out = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int in = x >= 150 && x < 260 && y >= 100 && y < 180;
            if (in && buf[y * W + x] != whole[y * W + x]) bad_in++;
            if (!in && buf[y * W + x]) bad_out++;
        }
    CHECK(!bad_in, "clip: %d pixels inside differ from the unclipped drawing", bad_in);
    CHECK(!bad_out, "clip: %d pixels drawn outside", bad_out);
    if (!bad_in && !bad_out) printf("ok: clipping changes nothing inside and draws nothing outside\n");
    rast_path_free(&p);

    /* Far bigger than the target, and far away -- as far as 16.16 reaches. */
    rast_path_init(&p);
    rast_rect(&p, F(-16000), F(-16000), F(32000), F(32000));
    clear(); fill(&p, RAST_NONZERO);
    near(area(), W * H, 0.0, "huge rectangle covers everything");
    rast_path_reset(&p);
    rast_rect(&p, F(20000), F(20000), F(100), F(100));
    clear(); fill(&p, RAST_NONZERO);
    CHECK(area() == 0, "far-away rectangle draws nothing");
    rast_path_free(&p);
}

static void t_strokes(void) {
    rast_path p; rast_path_init(&p);
    rast_stroke s;

    /* One pixel wide, along the middle of a row: exactly that row. */
    rast_move_to(&p, F(10), F(20.5));
    rast_line_to(&p, F(90), F(20.5));
    rast_stroke_init(&s, F(1));
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    CHECK(cov(50, 20) == 255 && cov(50, 19) == 0 && cov(50, 21) == 0,
          "1px line on a pixel row: %d %d %d", cov(50, 19), cov(50, 20), cov(50, 21));
    near(area(), 80, 0.001, "1px line, butt caps, area");

    s.cap = RAST_CAP_SQUARE;
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    near(area(), 81, 0.001, "1px line, square caps, area");

    rast_path_reset(&p);
    rast_move_to(&p, F(100), F(100));
    rast_line_to(&p, F(300), F(100));
    rast_stroke_init(&s, F(20));
    s.cap = RAST_CAP_ROUND;
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    near(area(), 200 * 20 + M_PI * 100, 0.003, "20px line, round caps, area");

    /* A zero-length line with round caps is a dot. */
    rast_path_reset(&p);
    rast_move_to(&p, F(50), F(50));
    rast_line_to(&p, F(50), F(50));
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    near(area(), M_PI * 100, 0.01, "zero-length line, round cap: a dot");

    /* A closed square: the ring between the two outlines, corners mitred. */
    rast_path_reset(&p);
    rast_rect(&p, F(100), F(100), F(100), F(80));
    rast_stroke_init(&s, F(10));
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    near(area(), 110 * 90 - 90 * 70, 0.001, "rectangle stroked, miter joins, area");
    CHECK(cov(96, 96) == 255 && cov(150, 140) == 0, "miter corner filled, middle empty");
    s.join = RAST_JOIN_BEVEL;
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    near(area(), 110 * 90 - 90 * 70 - 4 * 12.5, 0.002, "rectangle stroked, bevel joins, area");
    s.join = RAST_JOIN_ROUND;
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    near(area(), 110 * 90 - 90 * 70 - 4 * (25 - M_PI * 25 / 4), 0.003,
         "rectangle stroked, round joins, area");

    /* A sharp V: a miter past the limit falls back to a bevel. */
    rast_path_reset(&p);
    /* legs 15 degrees apart: a miter 7.5 widths long */
    rast_move_to(&p, F(100), F(250));
    rast_line_to(&p, F(200), F(50));
    rast_line_to(&p, F(160), F(250));
    rast_stroke_init(&s, F(10));
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    CHECK(cov(204, 35) == 0, "sharp miter cut off at the limit (%d)", cov(204, 35));
    s.miter_limit = F(100);
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    CHECK(cov(204, 35) == 255, "sharp miter kept with a high limit (%d)", cov(204, 35));

    /* Dashes: 10 on, 10 off along 100 px = five dashes of 10. */
    rast_path_reset(&p);
    rast_move_to(&p, F(0), F(10.5));
    rast_line_to(&p, F(100), F(10.5));
    rast_stroke_init(&s, F(1));
    rast_fx dash[2] = { F(10), F(10) };
    s.dash = dash; s.ndash = 2;
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    near(area(), 50, 0.001, "dashed line, area");
    CHECK(cov(5, 10) == 255 && cov(15, 10) == 0 && cov(25, 10) == 255 && cov(95, 10) == 0,
          "dashes where they belong");
    s.dash_offset = F(5);
    clear(); rast_draw_stroke(&T, &p, NULL, &s, &black);
    CHECK(cov(2, 10) == 255 && cov(7, 10) == 0 && cov(12, 10) == 0 && cov(17, 10) == 255,
          "dash offset shifts the pattern");

    /* Stroke through a non-uniform scale: the pen stretches too. */
    rast_path_reset(&p);
    rast_move_to(&p, F(10), F(10));
    rast_line_to(&p, F(10), F(40));
    rast_stroke_init(&s, F(2));
    rast_mat m = rast_scale(F(4), F(1));
    clear(); rast_draw_stroke(&T, &p, &m, &s, &black);
    near(area(), 8 * 30, 0.002, "vertical line under scale(4,1): 8 wide");
    rast_path_free(&p);
}

static void t_gradients(void) {
    rast_stop st[2] = { { 0, 0xFFFF0000 }, { RAST_ONE, 0xFF0000FF } };
    rast_paint g;
    memset(&g, 0, sizeof g);
    g.type = RAST_LINEAR;
    g.m = rast_identity();
    g.x1 = F(0); g.y1 = 0; g.x2 = F(255); g.y2 = 0;
    g.stops = st; g.nstops = 2; g.opacity = 255; g.spread = RAST_PAD;
    rast_path p; rast_path_init(&p);
    rast_rect(&p, 0, 0, F(W), F(10));
    for (int i = 0; i < W * H; i++) buf[i] = 0xFF000000;
    rast_target X = T;
    X.format = RAST_XRGB;
    rast_fill(&X, &p, NULL, RAST_NONZERO, &g);
    int worst = 0;
    for (int x = 0; x < 255; x++) {
        int r = (buf[5 * W + x] >> 16) & 255, b = buf[5 * W + x] & 255;
        int er = abs(r - (255 - x)), eb = abs(b - x);
        if (er > worst) worst = er;
        if (eb > worst) worst = eb;
    }
    CHECK(worst <= 2, "linear gradient off by up to %d", worst);
    if (worst <= 2) printf("ok: linear gradient red to blue, within %d of exact\n", worst);
    CHECK((buf[5 * W + 350] & 0xFFFFFF) == 0x0000FF, "pad beyond the end: %06x", buf[5 * W + 350] & 0xFFFFFF);

    g.spread = RAST_REFLECT;
    rast_fill(&X, &p, NULL, RAST_NONZERO, &g);
    int r300 = (buf[5 * W + 300] >> 16) & 255;
    CHECK(abs(r300 - 45) <= 3, "reflect: past the end it comes back (%d)", r300);

    /* Radial: centre colour at the focus, end colour on the circle. */
    g.type = RAST_RADIAL;
    g.cx = g.fx = F(200); g.cy = g.fy = F(150); g.r = F(100);
    g.spread = RAST_PAD;
    rast_path_reset(&p);
    rast_rect(&p, 0, 0, F(W), F(H));
    rast_fill(&X, &p, NULL, RAST_NONZERO, &g);
    /* Pixel centres are half a pixel off the exact points, so "near". */
    uint32_t c0 = buf[150 * W + 200];
    CHECK(((c0 >> 16) & 255) >= 250 && (c0 & 255) <= 5, "radial centre: %06x", c0 & 0xFFFFFF);
    uint32_t mid = buf[150 * W + 250];
    CHECK(abs((int)((mid >> 16) & 255) - 128) <= 3 && abs((int)(mid & 255) - 127) <= 3,
          "radial halfway: %06x", mid & 0xFFFFFF);
    CHECK((buf[150 * W + 320] & 0xFFFFFF) == 0x0000FF, "radial outside: %06x", buf[150 * W + 320] & 0xFFFFFF);
    /* Off-centre focus: t is 0 at the focus and still 1 on the circle. */
    g.fx = F(170);
    rast_fill(&X, &p, NULL, RAST_NONZERO, &g);
    uint32_t cf = buf[150 * W + 170];
    CHECK(((cf >> 16) & 255) >= 250 && (cf & 255) <= 5, "radial focus: %06x", cf & 0xFFFFFF);
    CHECK((buf[150 * W + 98] & 0xFFFFFF) == 0x0000FF, "radial circle edge with focus: %06x",
          buf[150 * W + 98] & 0xFFFFFF);
    uint32_t ce = buf[150 * W + 101];
    CHECK((ce & 255) >= 245, "radial just inside the circle, focus off-centre: %06x", ce & 0xFFFFFF);
    printf("ok: radial gradients (centre, halfway, outside, focus)\n");
    rast_path_free(&p);
}

static void t_random(void) {
    /* Anything at all, under the sanitizers: must not crash or overflow,
     * and coverage can never exceed full. */
    rast_path p; rast_path_init(&p);
    srand(7);
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    for (int iter = 0; iter < 3000; iter++) {
        rast_path_reset(&p);
        int n = 1 + rand() % 12;
        double span = (iter % 3 == 0) ? 30000 : (iter % 3 == 1) ? 600 : 50;
        for (int i = 0; i < n; i++) {
            rast_fx x = F((rand() / (double)RAND_MAX - 0.3) * span);
            rast_fx y = F((rand() / (double)RAND_MAX - 0.3) * span);
            int k = rand() % 5;
            if (i == 0 || k == 0) rast_move_to(&p, x, y);
            else if (k == 1) rast_line_to(&p, x, y);
            else if (k == 2) rast_quad_to(&p, y, x, x, y);
            else if (k == 3) rast_cubic_to(&p, x, x, y, y, x / 2, y / 2);
            else rast_arc_to(&p, x / 3, y / 3, x, rand() & 1, rand() & 1, y, x);
            if (rand() % 4 == 0) rast_close(&p);
        }
        clear();
        rast_mat m = rast_mul(rast_rotate(F(rand() % 360)), rast_scale(F(0.1 + rand() % 30 / 10.0), F(1)));
        rast_fill(&T, &p, &m, rand() & 1, &black);
        rast_stroke s;
        rast_stroke_init(&s, F(rand() % 40 / 4.0));
        s.join = rand() % 3; s.cap = rand() % 3;
        rast_fx dash[3] = { F(rand() % 10), F(1 + rand() % 10), F(rand() % 5) };
        if (rand() & 1) { s.dash = dash; s.ndash = 1 + rand() % 3; }
        rast_draw_stroke(&T, &p, &m, &s, &black);
    }
    double dt = (double)clock() / CLOCKS_PER_SEC - t0;
    printf("ok: 3000 random paths filled and stroked, no faults (%.2f s)\n", dt);
    rast_path_free(&p);
}

static void t_speed(void) {
    static uint32_t big[1920 * 1080];
    rast_target t;
    rast_target_init(&t, big, 1920, 1080, 1920, RAST_XRGB);
    rast_path p; rast_path_init(&p);
    rast_ellipse(&p, F(960), F(540), F(500), F(400));
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    for (int i = 0; i < 20; i++) rast_fill(&t, &p, NULL, RAST_NONZERO, &black);
    double dt = ((double)clock() / CLOCKS_PER_SEC - t0) / 20;
    printf("speed: a 1000x800 ellipse in %.2f ms\n", dt * 1000);
    rast_path_free(&p);
}

/* The test sheet: every feature once, for looking at. */
static void sheet(const char *dir) {
    enum { SW = 800, SH = 560 };
    static uint32_t px[SW * SH];
    for (int i = 0; i < SW * SH; i++) px[i] = 0xFFFFFF;
    rast_target t;
    rast_target_init(&t, px, SW, SH, SW, RAST_XRGB);
    rast_path p; rast_path_init(&p);
    rast_paint c;
    rast_stroke s;

    /* joins and caps */
    const char *names = "miter round bevel";
    (void)names;
    for (int j = 0; j < 3; j++) {
        rast_path_reset(&p);
        rast_move_to(&p, F(40 + j * 130), F(120));
        rast_line_to(&p, F(80 + j * 130), F(40));
        rast_line_to(&p, F(120 + j * 130), F(120));
        rast_stroke_init(&s, F(18));
        s.join = j; s.cap = j == 0 ? RAST_CAP_BUTT : j == 1 ? RAST_CAP_ROUND : RAST_CAP_SQUARE;
        rast_paint_solid(&c, 0xFF3060C0);
        rast_draw_stroke(&t, &p, NULL, &s, &c);
        rast_stroke_init(&s, F(1));
        rast_paint_solid(&c, 0xFFFF6000);
        rast_draw_stroke(&t, &p, NULL, &s, &c);
    }
    /* thin lines at many angles */
    for (int a = 0; a < 36; a++) {
        rast_path_reset(&p);
        double r = a * M_PI / 36;
        rast_move_to(&p, F(480), F(90));
        rast_line_to(&p, F(480 + 80 * cos(r * 2)), F(90 + 80 * sin(r * 2)));
        rast_stroke_init(&s, F(1));
        rast_paint_solid(&c, 0xFF000000);
        rast_draw_stroke(&t, &p, NULL, &s, &c);
    }
    /* dashes, round caps */
    rast_path_reset(&p);
    rast_move_to(&p, F(600), F(30));
    rast_cubic_to(&p, F(800), F(30), F(560), F(160), F(780), F(160));
    rast_fx d[2] = { F(12), F(8) };
    rast_stroke_init(&s, F(6));
    s.dash = d; s.ndash = 2; s.cap = RAST_CAP_ROUND;
    rast_paint_solid(&c, 0xFF208040);
    rast_draw_stroke(&t, &p, NULL, &s, &c);

    /* a pentagram both ways */
    for (int k = 0; k < 2; k++) {
        rast_path_reset(&p);
        for (int i = 0; i < 5; i++) {
            double a = -M_PI / 2 + i * 4 * M_PI / 5;
            rast_fx x = F(90 + k * 170 + 70 * cos(a)), y = F(280 + 70 * sin(a));
            if (i == 0) rast_move_to(&p, x, y); else rast_line_to(&p, x, y);
        }
        rast_close(&p);
        rast_paint_solid(&c, 0xFFC02040);
        rast_fill(&t, &p, NULL, k ? RAST_EVENODD : RAST_NONZERO, &c);
    }
    /* linear and radial gradients, translucent overlap */
    rast_stop st[3] = { { 0, 0xFFFFD000 }, { F(0.5), 0xFFE02060 }, { RAST_ONE, 0xFF2040E0 } };
    rast_paint g;
    memset(&g, 0, sizeof g);
    g.type = RAST_LINEAR; g.m = rast_identity();
    g.x1 = F(420); g.y1 = F(220); g.x2 = F(560); g.y2 = F(340);
    g.stops = st; g.nstops = 3; g.opacity = 255;
    rast_path_reset(&p);
    rast_round_rect(&p, F(410), F(210), F(160), F(140), F(24), F(24));
    rast_fill(&t, &p, NULL, RAST_NONZERO, &g);
    g.type = RAST_RADIAL; g.cx = F(690); g.cy = F(280); g.r = F(80);
    g.fx = F(660); g.fy = F(250);
    rast_path_reset(&p);
    rast_ellipse(&p, F(690), F(280), F(80), F(80));
    rast_fill(&t, &p, NULL, RAST_NONZERO, &g);
    rast_path_reset(&p);
    rast_ellipse(&p, F(560), F(300), F(60), F(60));
    rast_paint_solid(&c, 0x8020A020);
    rast_fill(&t, &p, NULL, RAST_NONZERO, &c);

    /* rotated, skewed text-like curves */
    for (int k = 0; k < 6; k++) {
        rast_path_reset(&p);
        rast_move_to(&p, F(0), F(0));
        rast_cubic_to(&p, F(30), F(-40), F(60), F(40), F(90), F(0));
        rast_quad_to(&p, F(45), F(60), F(0), F(0));
        rast_close(&p);
        rast_mat m = rast_mul(rast_translate(F(80 + k * 120), F(460)),
                              rast_mul(rast_rotate(F(k * 30)), rast_skew(F(k * 5), 0)));
        rast_paint_solid(&c, 0xFF000000 | (uint32_t)(k * 40) << 16 | 0x4080);
        rast_fill(&t, &p, &m, RAST_NONZERO, &c);
        rast_stroke_init(&s, F(2));
        rast_paint_solid(&c, 0xFF000000);
        rast_draw_stroke(&t, &p, &m, &s, &c);
    }
    rast_path_free(&p);

    char name[256];
    snprintf(name, sizeof name, "%s/sheet.ppm", dir);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    for (int i = 0; i < SW * SH; i++) {
        unsigned char rgb[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("sheet: %s\n", name);
}

int main(int argc, char **argv) {
    rast_target_init(&T, buf, W, H, W, RAST_ARGB);
    rast_paint_solid(&black, 0xFF000000);
    t_fractional_rect();
    t_circle();
    t_polygons();
    t_rules();
    t_clip();
    t_strokes();
    t_gradients();
    t_random();
    t_speed();
    if (argc > 1) sheet(argv[1]);
    printf(fails ? "\n%d FAILED\n" : "\nALL OK\n", fails);
    return fails != 0;
}
