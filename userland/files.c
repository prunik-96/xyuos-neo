/* files -- the file manager; and, started with --pick, the window every
 * program opens and saves files through.
 *
 * One program for both on purpose. A Save dialog that is its own little
 * browser is always the poorer one: it cannot rename the folder you are about
 * to save into, does not know the drive you just plugged in, and looks like a
 * different system. Here the dialog IS the file manager, with a name field and
 * two buttons along the bottom.
 *
 * Text is drawn with libtext (FreeType and HarfBuzz behind it), so names in
 * any script come out as they should; the icons are drawn here, from shapes,
 * so they are sharp at every size the views use. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include "gui.h"
#include "text.h"

#define PATHMAX 1024

/* --- language ------------------------------------------------------------- */

static int en;
#define T(ru, eng) (en ? (eng) : (ru))

/* --- the surface and the text on it --------------------------------------- */

static gui_t g;
static int have_txt;
static unsigned char last_mods;     /* the modifiers of the last key: Ctrl+click, Shift+click */
static txt_target tt;
static txt_style F_UI    = { TXT_SANS, 400, 0, 14 * 64 };
static txt_style F_BOLD  = { TXT_SANS, 600, 0, 14 * 64 };
static txt_style F_SMALL = { TXT_SANS, 400, 0, 12 * 64 };
static txt_style F_SBOLD = { TXT_SANS, 700, 0, 11 * 64 };
static txt_style F_HEAD  = { TXT_SANS, 600, 0, 12 * 64 };
static txt_style F_BIG   = { TXT_SANS, 600, 0, 17 * 64 };

static void clip_all(void) { tt.x0 = 0; tt.y0 = 0; tt.x1 = g.w; tt.y1 = g.h; }
static void clip_to(int x, int y, int w, int h) {
    tt.x0 = x < 0 ? 0 : x;
    tt.y0 = y < 0 ? 0 : y;
    tt.x1 = x + w > g.w ? g.w : x + w;
    tt.y1 = y + h > g.h ? g.h : y + h;
}

static int tw(const txt_style *st, const char *s) {
    if (!*s) return 0;
    if (have_txt) return txt_width(st, s, strlen(s));
    return gui_tw(&g, s, 1);
}

static int line_asc(const txt_style *st) {
    int a = 12, d = 4;
    if (have_txt) txt_metrics(st, &a, &d);
    return a;
}
static int line_desc(const txt_style *st) {
    int a = 12, d = 4;
    if (have_txt) txt_metrics(st, &a, &d);
    return d;
}

/* Text in a box `h` high whose top is at y, centred in it vertically. */
static void td(const txt_style *st, int x, int y, int h, const char *s, unsigned col) {
    if (!*s) return;
    if (!have_txt) { gui_text(&g, x, y + (h - g.fh) / 2, s, col); return; }
    int a = line_asc(st), d = line_desc(st);
    int base = y + (h - (a + d)) / 2 + a;
    tt.px = g.px;
    tt.stride = g.w;
    txt_draw(&tt, st, x, base, col, s, strlen(s));
}

/* The same, cut to `maxw` with an ellipsis. A file name keeps its extension:
 * "Очень длинное и…txt" says more than "Очень длинное имя ф…". */
static void td_fit(const txt_style *st, int x, int y, int h, const char *s, int maxw, unsigned col) {
    if (maxw <= 0) return;
    if (tw(st, s) <= maxw) { td(st, x, y, h, s, col); return; }
    static char buf[600];
    const char *ell = "\xE2\x80\xA6";
    int ew = tw(st, ell);
    const char *dot = strrchr(s, '.');
    size_t len = strlen(s);
    const char *tail = (dot && dot != s && len - (size_t)(dot - s) <= 6) ? dot + 1 : 0;
    int tailw = tail ? tw(st, tail) : 0;
    int room = maxw - ew - tailw;
    if (room < 8) { tail = 0; tailw = 0; room = maxw - ew; }
    int at = 0;
    size_t cut = have_txt ? txt_hit(st, s, tail ? (size_t)(dot - s) : len, room, &at) : (size_t)(room / (g.fw ? g.fw : 8));
    if (cut > sizeof buf - 16) cut = sizeof buf - 16;
    memcpy(buf, s, cut);
    strcpy(buf + cut, ell);
    if (tail) strcat(buf, tail);
    td(st, x, y, h, buf, col);
}

/* --- colour ---------------------------------------------------------------- */

static unsigned mix(unsigned a, unsigned b, int t) { return gui_mix(a, b, t); }
static int dark_theme(void) {
    unsigned c = GC_PANEL;
    return ((c >> 16 & 255) + (c >> 8 & 255) + (c & 255)) < 3 * 110;
}

/* --- shapes ---------------------------------------------------------------- */

static void px_blend(int x, int y, unsigned c, int a) {
    if (x < tt.x0 || y < tt.y0 || x >= tt.x1 || y >= tt.y1 || a <= 0) return;
    unsigned *p = g.px + (size_t)y * g.w + x;
    *p = a >= 255 ? c : mix(*p, c, a);
}

/* A filled rounded rectangle, its corners smoothed, at alpha a (0..255). */
static void rrect(int x, int y, int w, int h, int r, unsigned c, int a) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < tt.y0 || py >= tt.y1) continue;
        float cy = j < r ? (float)r - j - 0.5f : (j >= h - r ? (float)(j - (h - r)) + 0.5f : 0.f);
        int inner = cy <= 0.f;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < tt.x0 || px >= tt.x1) continue;
            int cov = 255;
            if (!inner) {
                float cx = i < r ? (float)r - i - 0.5f : (i >= w - r ? (float)(i - (w - r)) + 0.5f : 0.f);
                if (cx > 0.f) {
                    float k = (float)r - sqrtf(cx * cx + cy * cy) + 0.5f;
                    if (k <= 0.f) continue;
                    if (k < 1.f) cov = (int)(k * 255.f);
                }
            }
            px_blend(px, py, c, a * cov / 255);
        }
    }
}

/* Its outline, one pixel wide. */
static void rline(int x, int y, int w, int h, int r, unsigned c, int a) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        float cy = j < r ? (float)r - j - 0.5f : (j >= h - r ? (float)(j - (h - r)) + 0.5f : 0.f);
        for (int i = 0; i < w; i++) {
            float cx = i < r ? (float)r - i - 0.5f : (i >= w - r ? (float)(i - (w - r)) + 0.5f : 0.f);
            float dist;
            if (cx > 0.f && cy > 0.f) dist = (float)r - sqrtf(cx * cx + cy * cy);
            else {
                float dx = (float)(i < w - 1 - i ? i : w - 1 - i);
                float dy = (float)(j < h - 1 - j ? j : h - 1 - j);
                dist = (dx < dy ? dx : dy) + 0.5f;
                if (cx > 0.f) dist = (float)(j < h - 1 - j ? j : h - 1 - j) + 0.5f;
                if (cy > 0.f) dist = (float)(i < w - 1 - i ? i : w - 1 - i) + 0.5f;
            }
            float cov = 1.f - fabsf(dist - 0.5f);
            if (cov <= 0.f) continue;
            px_blend(x + i, y + j, c, (int)(a * cov));
        }
    }
}

static float min3(float a, float b, float c) { float m = a < b ? a : b; return m < c ? m : c; }
static float max3(float a, float b, float c) { float m = a > b ? a : b; return m > c ? m : c; }

/* A filled triangle, smoothed by sampling each pixel four by four. */
static void tri(float x0, float y0, float x1, float y1, float x2, float y2, unsigned c, int a) {
    int minx = (int)floorf(min3(x0, x1, x2)), maxx = (int)ceilf(max3(x0, x1, x2));
    int miny = (int)floorf(min3(y0, y1, y2)), maxy = (int)ceilf(max3(y0, y1, y2));
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0.f) return;
    float sg = area > 0 ? 1.f : -1.f;
    for (int y = miny; y < maxy; y++)
        for (int x = minx; x < maxx; x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4.f, py = y + (sy + 0.5f) / 4.f;
                    float e0 = ((x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)) * sg;
                    float e1 = ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)) * sg;
                    float e2 = ((x0 - x2) * (py - y2) - (y0 - y2) * (px - x2)) * sg;
                    if (e0 >= 0 && e1 >= 0 && e2 >= 0) in++;
                }
            if (in) px_blend(x, y, c, a * in / 16);
        }
}

static void disc(float cx, float cy, float r, unsigned c, int a) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float k = r - d + 0.5f;
            if (k <= 0) continue;
            px_blend(x, y, c, k >= 1 ? a : (int)(a * k));
        }
}

/* A soft shadow under a card. */
static void shadow(int x, int y, int w, int h, int r, int spread, int strength) {
    for (int k = spread; k >= 1; k--)
        rrect(x - k, y - k + spread / 3, w + 2 * k, h + 2 * k, r + k, 0x000000,
              strength * (spread - k + 1) / (spread * spread));
}

/* --- kinds of file, and their icons ------------------------------------------ */

enum { K_DIR, K_DRIVE, K_USB, K_TEXT, K_CODE, K_IMAGE, K_AUDIO, K_VIDEO, K_ARCHIVE,
       K_PDF, K_APP, K_WEB, K_DOC, K_FILE };

static int ext_is(const char *name, const char *ext) {
    const char *d = strrchr(name, '.');
    if (!d || d == name) return 0;
    d++;
    while (*d && *ext) {
        char a = *d, b = *ext;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return 0;
        d++; ext++;
    }
    return !*d && !*ext;
}

static int ext_any(const char *name, const char *list) {   /* "txt,md,log" */
    char e[16];
    while (*list) {
        int n = 0;
        while (*list && *list != ',' && n < 15) e[n++] = *list++;
        e[n] = 0;
        if (*list == ',') list++;
        if (n && ext_is(name, e)) return 1;
    }
    return 0;
}

static int kind_of(const char *name, int dir, const char *in_dir) {
    if (dir) return K_DIR;
    if (ext_any(name, "txt,md,log,ini,cfg,conf,csv,json,xml,yml,yaml")) return K_TEXT;
    if (ext_any(name, "c,h,cpp,hpp,cc,py,js,ts,sh,bat,s,asm,rs,go,java,lua,mk")) return K_CODE;
    if (ext_any(name, "png,jpg,jpeg,bmp,gif,webp,ppm,svg,ico,tga")) return K_IMAGE;
    if (ext_any(name, "mp3,wav,ogg,flac,m4a,aac,opus")) return K_AUDIO;
    if (ext_any(name, "mp4,mkv,avi,mov,webm")) return K_VIDEO;
    if (ext_any(name, "zip,tar,gz,7z,rar,xz,bz2,iso,img")) return K_ARCHIVE;
    if (ext_is(name, "pdf")) return K_PDF;
    if (ext_any(name, "html,htm,css")) return K_WEB;
    if (ext_any(name, "doc,docx,odt,rtf,xls,xlsx,ppt,pptx")) return K_DOC;
    if (ext_is(name, "elf") || (in_dir && strcmp(in_dir, "/bin") == 0 && !strchr(name, '.')))
        return K_APP;
    return K_FILE;
}

static const char *kind_name(int k, const char *name) {
    static char buf[48];
    switch (k) {
    case K_DIR:     return T("Папка", "Folder");
    case K_DRIVE:   return T("Диск", "Drive");
    case K_USB:     return T("Флешка", "USB drive");
    case K_TEXT:    return T("Текст", "Text");
    case K_CODE:    return T("Исходный код", "Source code");
    case K_IMAGE:   return T("Изображение", "Image");
    case K_AUDIO:   return T("Аудио", "Audio");
    case K_VIDEO:   return T("Видео", "Video");
    case K_ARCHIVE: return T("Архив", "Archive");
    case K_PDF:     return "PDF";
    case K_APP:     return T("Программа", "Program");
    case K_WEB:     return T("Веб-страница", "Web page");
    case K_DOC:     return T("Документ", "Document");
    }
    const char *d = strrchr(name, '.');
    if (d && d != name && strlen(d + 1) <= 8) {
        int n = 0;
        for (const char *p = d + 1; *p && n < 10; p++) buf[n++] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
        buf[n] = 0;
        snprintf(buf + n, sizeof buf - n, T("-файл", " file"));
        return buf;
    }
    return T("Файл", "File");
}

static unsigned kind_color(int k) {
    switch (k) {
    case K_TEXT:    return 0x5B7A99;
    case K_CODE:    return 0x2E9E6A;
    case K_IMAGE:   return 0x8A5CD6;
    case K_AUDIO:   return 0xE8743B;
    case K_VIDEO:   return 0xD64545;
    case K_ARCHIVE: return 0xA9792E;
    case K_PDF:     return 0xD23B3B;
    case K_APP:     return 0x2A6FD6;
    case K_WEB:     return 0x1E88C7;
    case K_DOC:     return 0x2B5FB4;
    }
    return 0x8A94A3;
}

static const char *kind_badge(int k, const char *name) {
    static char b[6];
    switch (k) {
    case K_PDF: return "PDF";
    case K_ARCHIVE: return "ZIP";
    case K_WEB: return "HTML";
    }
    const char *d = strrchr(name, '.');
    if (!d || d == name) return "";
    int n = 0;
    for (const char *p = d + 1; *p && n < 4; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((unsigned char)c >= 0x80) return "";
        b[n++] = c;
    }
    b[n] = 0;
    return b;
}

static void icon_folder(int x, int y, int s, unsigned col) {
    unsigned back = mix(col, 0x000000, 50), front = col;
    rrect(x + s * 6 / 100, y + s * 16 / 100, s * 44 / 100, s * 22 / 100, s / 14 + 1, back, 255);
    rrect(x + s * 4 / 100, y + s * 24 / 100, s * 92 / 100, s * 64 / 100, s / 10 + 1, back, 255);
    rrect(x + s * 4 / 100, y + s * 32 / 100, s * 92 / 100, s * 56 / 100, s / 10 + 1, front, 255);
    rrect(x + s * 4 / 100, y + s * 32 / 100, s * 92 / 100, s / 12 + 1, s / 10 + 1, 0xFFFFFF, 50);
}

/* A sheet with its top-right corner folded down. */
static void page_shape(int px, int py, int pw, int ph, int fold, int r, unsigned c) {
    rrect(px, py + fold, pw, ph - fold, r, c, 255);
    rrect(px, py, pw - fold, ph - fold + r * 2, r, c, 255);
    rrect(px + pw - fold, py + fold, fold, r + 1, 0, c, 255);
    tri((float)(px + pw - fold), (float)py, (float)(px + pw), (float)(py + fold),
        (float)(px + pw - fold), (float)(py + fold), c, 255);
}

static void icon_page(int x, int y, int s, int k, const char *name) {
    int px = x + s * 18 / 100, py = y + s * 6 / 100, pw = s * 64 / 100, ph = s * 88 / 100;
    int fold = s * 22 / 100, r = s / 14 + 1;
    unsigned paper = dark_theme() ? 0xE9EDF2 : 0xFFFFFF;
    page_shape(px - 1, py - 1, pw + 2, ph + 2, fold + 1, r + 1, mix(0x9AA5B1, GC_PANEL, 90));
    page_shape(px, py, pw, ph, fold, r, paper);
    /* the folded corner's flap */
    tri((float)(px + pw - fold), (float)py, (float)(px + pw - fold), (float)(py + fold),
        (float)(px + pw), (float)(py + fold), 0xC9D1DA, 255);
    unsigned kc = kind_color(k);
    if (k == K_IMAGE) {
        int ix = px + s * 8 / 100, iy = py + s * 34 / 100, iw = pw - s * 16 / 100, ih = s * 36 / 100;
        rrect(ix, iy, iw, ih, s / 20 + 1, 0xDDEBFF, 255);
        tri((float)ix, (float)(iy + ih), (float)(ix + iw * 45 / 100), (float)(iy + ih * 30 / 100),
            (float)(ix + iw * 80 / 100), (float)(iy + ih), 0x4CAF7D, 255);
        tri((float)(ix + iw * 45 / 100), (float)(iy + ih), (float)(ix + iw * 72 / 100), (float)(iy + ih * 52 / 100),
            (float)(ix + iw), (float)(iy + ih), 0x2E8B5E, 255);
        disc((float)(ix + iw * 75 / 100), (float)(iy + ih * 28 / 100), (float)s * 6 / 100, 0xF4B83A, 255);
        return;
    }
    if (k == K_AUDIO) {
        float cx = (float)(px + pw / 2), cy = (float)(py + ph * 60 / 100);
        disc(cx - s * 0.08f, cy + s * 0.08f, s * 0.07f, kc, 255);
        rrect((int)(cx - s * 0.02f), (int)(cy - s * 0.22f), s / 22 + 1, (int)(s * 0.3f), 1, kc, 255);
        rrect((int)(cx - s * 0.02f), (int)(cy - s * 0.22f), (int)(s * 0.16f), s / 16 + 1, 1, kc, 255);
        return;
    }
    if (k == K_VIDEO) {
        float cx = (float)(px + pw / 2), cy = (float)(py + ph * 58 / 100);
        tri(cx - s * 0.1f, cy - s * 0.13f, cx + s * 0.14f, cy, cx - s * 0.1f, cy + s * 0.13f, kc, 255);
        return;
    }
    /* lines of text, and a band with the type on it */
    if (s >= 24) {
        for (int i = 0; i < 3; i++)
            rrect(px + s * 10 / 100, py + s * (26 + i * 9) / 100, pw - s * (i == 2 ? 30 : 20) / 100,
                  s / 30 + 1, 1, 0xB8C1CC, 255);
    }
    const char *badge = kind_badge(k, name);
    if (k == K_FILE && !badge[0]) return;
    int bx = x + s * 8 / 100, by = py + ph * 60 / 100, bw = s * 60 / 100, bh = s * 26 / 100;
    if (s < 24) { bx = px; by = py + ph - s * 30 / 100; bw = pw; bh = s * 18 / 100 + 1; }
    rrect(bx, by, bw, bh, s / 16 + 1, kc, 255);
    if (s >= 40 && badge[0]) {
        txt_style st = F_SBOLD;
        st.size = s * 64 * 17 / 100;
        int w = tw(&st, badge);
        if (w > bw - 4) { st.size = st.size * (bw - 4) / w; w = tw(&st, badge); }
        td(&st, bx + (bw - w) / 2, by, bh, badge, 0xFFFFFF);
    }
}

static void icon_drive(int x, int y, int s, int usb) {
    if (usb) {
        int bx = x + s * 30 / 100, by = y + s * 30 / 100, bw = s * 40 / 100, bh = s * 62 / 100;
        rrect(bx + s * 6 / 100, y + s * 8 / 100, bw - s * 12 / 100, s * 26 / 100, s / 20 + 1, 0xB9C3CF, 255);
        rrect(bx, by, bw, bh, s / 10 + 1, GC_ACCENT, 255);
        rrect(bx, by, bw, bh / 3, s / 10 + 1, 0xFFFFFF, 40);
        disc((float)(bx + bw / 2), (float)(by + bh * 70 / 100), (float)s * 5 / 100, 0xFFFFFF, 200);
        return;
    }
    int bx = x + s * 8 / 100, by = y + s * 26 / 100, bw = s * 84 / 100, bh = s * 50 / 100;
    rrect(bx, by, bw, bh, s / 8 + 1, 0x6E7A88, 255);
    rrect(bx, by, bw, bh / 2, s / 8 + 1, 0xFFFFFF, 40);
    disc((float)(bx + bw * 80 / 100), (float)(by + bh * 55 / 100), (float)s * 5 / 100 + 0.5f, 0x7CF29A, 255);
    rrect(bx + bw * 12 / 100, by + bh * 50 / 100, bw * 45 / 100, s / 20 + 1, 1, 0xFFFFFF, 90);
}

static void icon_app(int x, int y, int s) {
    rrect(x + s * 10 / 100, y + s * 10 / 100, s * 80 / 100, s * 80 / 100, s / 5, GC_ACCENT, 255);
    rrect(x + s * 10 / 100, y + s * 10 / 100, s * 80 / 100, s * 36 / 100, s / 5, 0xFFFFFF, 40);
    float cx = x + s * 0.5f, cy = y + s * 0.5f;
    tri(cx - s * 0.1f, cy - s * 0.16f, cx + s * 0.17f, cy, cx - s * 0.1f, cy + s * 0.16f, 0xFFFFFF, 240);
}

static void draw_icon(int k, int x, int y, int s, const char *name) {
    switch (k) {
    case K_DIR:   icon_folder(x, y, s, mix(GC_ACCENT, 0x5AB0F0, 120)); break;
    case K_DRIVE: icon_drive(x, y, s, 0); break;
    case K_USB:   icon_drive(x, y, s, 1); break;
    case K_APP:   icon_app(x, y, s); break;
    default:      icon_page(x, y, s, k, name); break;
    }
}

/* Small glyphs for the places in the side bar. */
static void glyph_place(int which, int x, int y, int s, unsigned col) {
    float fx = (float)x, fy = (float)y, f = (float)s;
    switch (which) {
    case 0:   /* home */
        tri(fx + f * 0.08f, fy + f * 0.5f, fx + f * 0.5f, fy + f * 0.1f, fx + f * 0.92f, fy + f * 0.5f, col, 255);
        rrect(x + s * 20 / 100, y + s * 46 / 100, s * 60 / 100, s * 44 / 100, 2, col, 255);
        rrect(x + s * 42 / 100, y + s * 62 / 100, s * 16 / 100, s * 28 / 100, 1, GC_BAR, 255);
        break;
    case 1:   /* documents */
        rrect(x + s * 18 / 100, y + s * 8 / 100, s * 64 / 100, s * 84 / 100, 2, col, 255);
        for (int i = 0; i < 3; i++)
            rrect(x + s * 30 / 100, y + s * (30 + i * 18) / 100, s * 40 / 100, s / 9 + 1, 1, GC_BAR, 255);
        break;
    case 2:   /* pictures */
        rrect(x + s * 6 / 100, y + s * 16 / 100, s * 88 / 100, s * 68 / 100, 3, col, 255);
        tri(fx + f * 0.16f, fy + f * 0.76f, fx + f * 0.42f, fy + f * 0.42f, fx + f * 0.68f, fy + f * 0.76f, GC_BAR, 255);
        disc(fx + f * 0.7f, fy + f * 0.36f, f * 0.09f, GC_BAR, 255);
        break;
    case 3:   /* music */
        disc(fx + f * 0.32f, fy + f * 0.74f, f * 0.16f, col, 255);
        rrect(x + s * 40 / 100, y + s * 12 / 100, s / 9 + 1, s * 62 / 100, 1, col, 255);
        rrect(x + s * 40 / 100, y + s * 12 / 100, s * 40 / 100, s / 7 + 1, 1, col, 255);
        break;
    case 4:   /* videos */
        rrect(x + s * 6 / 100, y + s * 18 / 100, s * 88 / 100, s * 64 / 100, 3, col, 255);
        tri(fx + f * 0.4f, fy + f * 0.34f, fx + f * 0.66f, fy + f * 0.5f, fx + f * 0.4f, fy + f * 0.66f, GC_BAR, 255);
        break;
    case 5:   /* downloads */
        rrect(x + s * 42 / 100, y + s * 6 / 100, s * 16 / 100, s * 46 / 100, 1, col, 255);
        tri(fx + f * 0.2f, fy + f * 0.46f, fx + f * 0.8f, fy + f * 0.46f, fx + f * 0.5f, fy + f * 0.78f, col, 255);
        rrect(x + s * 10 / 100, y + s * 84 / 100, s * 80 / 100, s / 9 + 1, 1, col, 255);
        break;
    }
}

/* --- paths ------------------------------------------------------------------- */

static void path_join(char *out, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0) snprintf(out, PATHMAX, "/%s", name);
    else snprintf(out, PATHMAX, "%s/%s", dir, name);
}

static void path_parent(char *out, const char *p) {
    snprintf(out, PATHMAX, "%s", p);
    char *s = strrchr(out, '/');
    if (!s || s == out) { strcpy(out, "/"); return; }
    *s = 0;
}

static const char *path_base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* The same drive: a rename can move a file between two of its folders; to
 * another drive it has to be copied. */
static void drive_of(const char *p, char *out) {
    if (strncmp(p, "/usb", 4) == 0) {
        int n = 4;
        while (p[n] && p[n] != '/') n++;
        memcpy(out, p, (size_t)n);
        out[n] = 0;
        return;
    }
    strcpy(out, "/");
}
static int same_drive(const char *a, const char *b) {
    char da[32], db[32];
    drive_of(a, da);
    drive_of(b, db);
    return strcmp(da, db) == 0;
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* --- the standard folders and their names ---------------------------------------- */

static const struct { const char *path, *ru, *en; int glyph; } stdf[] = {
    { "/home",           "Домашняя папка", "Home",      0 },
    { "/home/Documents", "Документы",      "Documents", 1 },
    { "/home/Pictures",  "Изображения",    "Pictures",  2 },
    { "/home/Music",     "Музыка",         "Music",     3 },
    { "/home/Videos",    "Видео",          "Videos",    4 },
    { "/home/Downloads", "Загрузки",       "Downloads", 5 },
};
#define NSTD ((int)(sizeof stdf / sizeof stdf[0]))

/* What a folder is called on screen: its own name, or the translated name of
 * a standard one -- "Documents" is "Документы" in a Russian desktop, as on
 * any system that has standard folders. */
static const char *disp_name(const char *full, const char *name) {
    for (int i = 0; i < NSTD; i++)
        if (strcmp(full, stdf[i].path) == 0) return T(stdf[i].ru, stdf[i].en);
    if (strcmp(full, "/") == 0) return T("Система", "System");
    return name;
}

/* --- the drives -------------------------------------------------------------------- */

static struct uvol vols[16];
static int nvols;
static unsigned vols_stamp;

static const char *vol_title(const struct uvol *v) {
    static char buf[80];
    if (v->index < 0) return T("Система", "System");
    if (v->label[0]) return v->label;
    if (v->drive[0]) { snprintf(buf, sizeof buf, "%s", v->drive); return buf; }
    return T("Флешка", "USB drive");
}

static void fmt_size(char *out, int max, unsigned long long b) {
    if (b < 1024) { snprintf(out, max, T("%llu Б", "%llu B"), b); return; }
    static const char *ru_u[] = { "КБ", "МБ", "ГБ", "ТБ" }, *en_u[] = { "KB", "MB", "GB", "TB" };
    double v = (double)b / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
    const char *un = en ? en_u[u] : ru_u[u];
    char num[32];
    if (v >= 100) snprintf(num, sizeof num, "%d", (int)(v + 0.5));
    else {
        int t = (int)(v * 10 + 0.5);
        snprintf(num, sizeof num, en ? "%d.%d" : "%d,%d", t / 10, t % 10);
    }
    snprintf(out, max, "%s %s", num, un);
}

static void refresh_volumes(void) {
    struct uvol nv[16];
    int n = volumes_list(nv, 16);
    if (n < 0) n = 0;
    /* Free space is counted once per volume (a FAT has to be read for it). */
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int k = 0; k < nvols; k++)
            if (strcmp(vols[k].mount, nv[i].mount) == 0 && strcmp(vols[k].label, nv[i].label) == 0 &&
                vols[k].total == nv[i].total && vols[k].free) { nv[i].free = vols[k].free; found = 1; }
        if (!found || nv[i].index < 0) {
            struct uvol sp;
            if (volume_space(nv[i].index, &sp) == 0) nv[i].free = sp.free;
        }
    }
    int changed = n != nvols;
    for (int i = 0; i < n && !changed; i++)
        if (strcmp(vols[i].mount, nv[i].mount) || strcmp(vols[i].label, nv[i].label) ||
            vols[i].free != nv[i].free) changed = 1;
    memcpy(vols, nv, sizeof nv);
    nvols = n;
    if (changed) vols_stamp++;
}

/* The volume a path is on, or NULL for the system. */
static const struct uvol *vol_for(const char *p) {
    char d[32];
    drive_of(p, d);
    for (int i = 0; i < nvols; i++) if (strcmp(vols[i].mount, d) == 0) return &vols[i];
    return 0;
}

/* --- the listing ---------------------------------------------------------------------- */

typedef struct {
    char name[256];
    char disp[256];
    unsigned long long size;
    long long mtime;
    int dir, mount, kind, sel;
} item_t;

static item_t *items;
static int nitems, cap_items;
static int *view;
static int nview;
static char cwd[PATHMAX] = "/home";
static int show_hidden;
static int sort_col;            /* 0 name, 1 date, 2 size, 3 type */
static int sort_desc;
static int grid;                /* 0 list, 1 icons */
static char search[256];
static char status_msg[256];
static unsigned long long status_until;

static void set_status(const char *s) {
    snprintf(status_msg, sizeof status_msg, "%s", s);
    status_until = uptime_ms() + 4000;
}

/* Comparing names as people read them: without regard to case, Russian too. */
static unsigned next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p;
    if (!c) return 0;
    if (c < 0x80) { (*s)++; return c; }
    if ((c >> 5) == 6 && p[1]) { (*s) += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((c >> 4) == 14 && p[1] && p[2]) { (*s) += 3; return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    (*s)++;
    return c;
}
static unsigned fold(unsigned c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c == 0x451) return 0x435;            /* ё sorts with е */
    return c;
}
static int name_cmp(const char *a, const char *b) {
    for (;;) {
        unsigned x = fold(next_cp(&a)), y = fold(next_cp(&b));
        /* digits compare as numbers: "file 2" before "file 10" */
        if (x >= '0' && x <= '9' && y >= '0' && y <= '9') {
            unsigned long long nx = x - '0', ny = y - '0';
            while (*a >= '0' && *a <= '9') nx = nx * 10 + (unsigned)(*a++ - '0');
            while (*b >= '0' && *b <= '9') ny = ny * 10 + (unsigned)(*b++ - '0');
            if (nx != ny) return nx < ny ? -1 : 1;
            continue;
        }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
static int contains_fold(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (const char *h = hay; *h; ) {
        const char *a = h, *b = needle;
        for (;;) {
            if (!*b) return 1;
            unsigned x = fold(next_cp(&a)), y = fold(next_cp(&b));
            if (!x || x != y) break;
        }
        next_cp(&h);
    }
    return 0;
}

static int cmp_items(const item_t *a, const item_t *b) {
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    int r = 0;
    switch (sort_col) {
    case 1: r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime; break;
    case 2: r = a->size < b->size ? -1 : a->size > b->size; break;
    case 3: {
        r = a->kind - b->kind;
        if (!r) {
            const char *ea = strrchr(a->name, '.'), *eb = strrchr(b->name, '.');
            r = name_cmp(ea ? ea : "", eb ? eb : "");
        }
        break;
    }
    }
    if (!r) r = name_cmp(a->disp, b->disp);
    return sort_desc ? -r : r;
}

static void build_view(void) {
    free(view);
    view = (int *)malloc(sizeof(int) * (size_t)(nitems + 1));
    nview = 0;
    for (int i = 0; i < nitems; i++) {
        if (!show_hidden && items[i].name[0] == '.') continue;
        if (search[0] && !contains_fold(items[i].disp, search)) continue;
        view[nview++] = i;
    }
    /* insertion sort: listings are small, and it keeps equal things in order */
    for (int i = 1; i < nview; i++) {
        int v = view[i], j = i - 1;
        while (j >= 0 && cmp_items(&items[view[j]], &items[v]) > 0) { view[j + 1] = view[j]; j--; }
        view[j + 1] = v;
    }
}

/* --- the picker's request ------------------------------------------------------------- */

#define PICK_NONE 0
#define PICK_OPEN 1
#define PICK_SAVE 2
#define PICK_DIR  3
static int pick;
static char pick_req[PATHMAX], pick_title[128], pick_exts[128], pick_label[96];

static int pick_accepts(const item_t *it) {
    if (it->dir || !pick || !pick_exts[0] || pick == PICK_DIR) return 1;
    return ext_any(it->name, pick_exts);
}

static int load(void) {
    static struct xdirent buf[2048];
    int n = readdir_x(cwd, buf, 2048);
    if (n < 0) return 0;
    if (n > 2048) n = 2048;
    if (n > cap_items) {
        item_t *ni = (item_t *)realloc(items, sizeof(item_t) * (size_t)n);
        if (!ni) return 0;
        items = ni;
        cap_items = n;
    }
    nitems = 0;
    for (int i = 0; i < n; i++) {
        struct xdirent *e = &buf[i];
        if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0) continue;
        /* The root's own "usb" folder is only where a drive goes: with the
         * drive in, it is listed once, as the drive. */
        if (strcmp(cwd, "/") == 0 && !(e->flags & XD_MOUNT) && strncmp(e->name, "usb", 3) == 0) {
            int dup = 0;
            for (int k = 0; k < n; k++) if ((buf[k].flags & XD_MOUNT) && strcmp(buf[k].name, e->name) == 0) dup = 1;
            if (dup) continue;
        }
        item_t *it = &items[nitems];
        memset(it, 0, sizeof *it);
        snprintf(it->name, sizeof it->name, "%s", e->name);
        it->size = e->size;
        it->mtime = e->mtime;
        it->dir = e->is_dir != 0;
        it->mount = (e->flags & XD_MOUNT) != 0;
        it->kind = it->mount ? K_USB : kind_of(it->name, it->dir, cwd);
        char full[PATHMAX];
        path_join(full, cwd, it->name);
        snprintf(it->disp, sizeof it->disp, "%s", disp_name(full, it->name));
        if (it->mount) {
            const struct uvol *v = vol_for(full);
            if (v) snprintf(it->disp, sizeof it->disp, "%s", vol_title(v));
        }
        if (pick == PICK_OPEN && !pick_accepts(it)) continue;
        if (pick == PICK_DIR && !it->dir) continue;
        nitems++;
    }
    build_view();
    return 1;
}

/* --- navigation ------------------------------------------------------------------------ */

#define HIST 64
static char hist[HIST][PATHMAX];
static int hist_n, hist_at;
static int scroll_y;            /* pixels */
static int cursor = -1;         /* view index the keyboard is on */
static int anchor = -1;
static int dirty = 1;

static void clear_sel(void) { for (int i = 0; i < nitems; i++) items[i].sel = 0; }
static int nsel(void) { int n = 0; for (int i = 0; i < nitems; i++) n += items[i].sel; return n; }

static void canon(char *p) {
    /* collapse "//", "/./", "/x/.." */
    char out[PATHMAX];
    int o = 0;
    const char *s = p;
    out[0] = 0;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        int n = (int)(e - s);
        if (n == 1 && s[0] == '.') { s = e; continue; }
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = 0;
            s = e;
            continue;
        }
        if (o + n + 2 >= PATHMAX) break;
        out[o++] = '/';
        memcpy(out + o, s, (size_t)n);
        o += n;
        out[o] = 0;
        s = e;
    }
    if (!o) strcpy(out, "/");
    strcpy(p, out);
}

static void go(const char *path, int remember) {
    char p[PATHMAX];
    snprintf(p, sizeof p, "%s", path);
    canon(p);
    if (!is_dir(p)) { set_status(T("Папка недоступна", "That folder cannot be opened")); return; }
    if (remember && strcmp(p, cwd) != 0) {
        if (hist_at < hist_n - 1) hist_n = hist_at + 1;
        if (hist_n == HIST) { memmove(hist[0], hist[1], sizeof hist[0] * (HIST - 1)); hist_n--; }
        snprintf(hist[hist_n++], PATHMAX, "%s", p);
        hist_at = hist_n - 1;
    }
    snprintf(cwd, sizeof cwd, "%s", p);
    search[0] = 0;
    load();
    scroll_y = 0;
    cursor = -1;
    anchor = -1;
    dirty = 1;
}

static void go_back(void) { if (hist_at > 0) { hist_at--; go(hist[hist_at], 0); } }
static void go_fwd(void)  { if (hist_at < hist_n - 1) { hist_at++; go(hist[hist_at], 0); } }
static void go_up(void) {
    if (strcmp(cwd, "/") == 0) return;
    char p[PATHMAX], was[256];
    snprintf(was, sizeof was, "%s", path_base(cwd));
    path_parent(p, cwd);
    go(p, 1);
    /* where we came from stays under the cursor */
    for (int i = 0; i < nview; i++) if (strcmp(items[view[i]].name, was) == 0) {
        cursor = anchor = i;
        items[view[i]].sel = 1;
    }
}

/* --- editing a line of text (rename, search, the picker's name) ------------------------ */

typedef struct {
    char s[256];
    int cur;           /* byte offset */
    int all;           /* the start is selected: typing replaces it ... */
    int sel_end;       /* ... up to here (0: all of it) -- a name without its extension */
    int active;
} ed_t;

/* Drop what is selected; the caret where it was. */
static void ed_cut_sel(ed_t *e) {
    int n = (int)strlen(e->s);
    int end = e->sel_end > 0 && e->sel_end < n ? e->sel_end : n;
    memmove(e->s, e->s + end, (size_t)(n - end + 1));
    e->cur = 0;
    e->all = 0;
    e->sel_end = 0;
}

static int ed_back(const ed_t *e, int at) {
    if (at <= 0) return 0;
    at--;
    while (at > 0 && ((unsigned char)e->s[at] & 0xC0) == 0x80) at--;
    return at;
}
static int ed_fwd(const ed_t *e, int at) {
    int n = (int)strlen(e->s);
    if (at >= n) return n;
    at++;
    while (at < n && ((unsigned char)e->s[at] & 0xC0) == 0x80) at++;
    return at;
}
static void ed_set(ed_t *e, const char *s, int select_all) {
    snprintf(e->s, sizeof e->s, "%s", s);
    e->cur = (int)strlen(e->s);
    e->all = select_all;
    e->sel_end = 0;
    e->active = 1;
}

/* A file's name to edit: its name selected, its extension left alone, as
 * Windows does -- typing a new name keeps ".txt". */
static void ed_set_name(ed_t *e, const char *s) {
    ed_set(e, s, 1);
    const char *d = strrchr(e->s, '.');
    if (d && d != e->s) e->sel_end = (int)(d - e->s);
}
/* A key for the line: 1 if it changed the text. Enter and Escape are the
 * caller's. */
static int ed_key(ed_t *e, const key_event_t *k) {
    int n = (int)strlen(e->s);
    unsigned char b = (unsigned char)k->ascii;
    if (k->code == XKEY_CHAR && (k->mods & XMOD_CTRL)) {
        if (k->ascii == 'a' || k->ascii == 'A') { e->all = 1; return 0; }
        if (k->ascii == 'v' || k->ascii == 'V') {
            char t[256];
            int type, len = clip_get(t, sizeof t - 1, &type);
            if (len > 0 && type == CLIP_TEXT) {
                if (len > (int)sizeof t - 1) len = (int)sizeof t - 1;
                t[len] = 0;
                for (char *p = t; *p; p++) if (*p == '\n' || *p == '\r' || *p == '/') *p = ' ';
                if (e->all) { ed_cut_sel(e); n = (int)strlen(e->s); }
                if (n + len < (int)sizeof e->s - 1) {
                    memmove(e->s + e->cur + len, e->s + e->cur, (size_t)(n - e->cur + 1));
                    memcpy(e->s + e->cur, t, (size_t)len);
                    e->cur += len;
                }
                return 1;
            }
            return 0;
        }
        if (k->ascii == 'c' || k->ascii == 'C') { clip_set(CLIP_TEXT, e->s, (unsigned)n); return 0; }
        return 0;
    }
    if (k->code == XKEY_CHAR && (b >= 32 || b >= 0x80) && b != 127 && !(k->mods & XMOD_ALT)) {
        if (e->all) { ed_cut_sel(e); n = (int)strlen(e->s); }
        if (n + 1 >= (int)sizeof e->s) return 0;
        memmove(e->s + e->cur + 1, e->s + e->cur, (size_t)(n - e->cur + 1));
        e->s[e->cur++] = k->ascii;
        return 1;
    }
    if (k->code == XKEY_BKSP) {
        if (e->all) { ed_cut_sel(e); return 1; }
        if (e->cur > 0) {
            int from = ed_back(e, e->cur);
            memmove(e->s + from, e->s + e->cur, (size_t)(n - e->cur + 1));
            e->cur = from;
            return 1;
        }
        return 0;
    }
    if (k->code == XKEY_DEL) {
        if (e->all) { ed_cut_sel(e); return 1; }
        if (e->cur < n) {
            int to = ed_fwd(e, e->cur);
            memmove(e->s + e->cur, e->s + to, (size_t)(n - to + 1));
            return 1;
        }
        return 0;
    }
    if (k->code == XKEY_LEFT)  { e->all = 0; e->cur = ed_back(e, e->cur); return 0; }
    if (k->code == XKEY_RIGHT) { e->all = 0; e->cur = ed_fwd(e, e->cur); return 0; }
    if (k->code == XKEY_HOME)  { e->all = 0; e->cur = 0; return 0; }
    if (k->code == XKEY_END)   { e->all = 0; e->cur = n; return 0; }
    return 0;
}

static void ed_draw(const ed_t *e, int x, int y, int w, int h, const txt_style *st, int focused, int blink) {
    rrect(x, y, w, h, 7, GC_PANEL, 255);
    rline(x, y, w, h, 7, focused ? GC_ACCENT : GC_EDGE, focused ? 255 : 180);
    clip_to(x + 6, y, w - 12, h);
    int tx = x + 10;
    /* keep the caret in sight */
    char pre[256];
    memcpy(pre, e->s, (size_t)e->cur);
    pre[e->cur] = 0;
    int cx = tw(st, pre);
    int shift = cx > w - 30 ? cx - (w - 30) : 0;
    if (e->all && e->s[0] && focused) {
        char sel[256];
        snprintf(sel, sizeof sel, "%.*s", e->sel_end > 0 ? e->sel_end : (int)strlen(e->s), e->s);
        rrect(tx - shift - 2, y + 5, tw(st, sel) + 4, h - 10, 3, GC_SEL, 255);
    }
    td(st, tx - shift, y, h, e->s, GC_TEXT);
    if (focused && blink && !e->all) rrect(tx - shift + cx, y + 7, 2, h - 14, 1, GC_ACCENT, 255);
    clip_all();
}

/* --- jobs: copying, moving, deleting ------------------------------------------------- */

typedef struct { char *src, *dst; int op; unsigned long long size; } step_t;
#define OP_MKDIR 0
#define OP_COPY  1
#define OP_RMFILE 2
#define OP_RMDIR 3
#define OP_RENAME 4

static step_t *steps;
static int nsteps, cap_steps, at_step;
static int job;                     /* 0 none, else what it is doing */
#define JOB_COPY 1
#define JOB_MOVE 2
#define JOB_DELETE 3
static unsigned long long job_total, job_done;
static int job_fail, job_cancel;
static int cp_in = -1, cp_out = -1;
static unsigned long long job_t0;
static char job_refresh[PATHMAX];

static void step_add(int op, const char *src, const char *dst, unsigned long long size) {
    if (nsteps == cap_steps) {
        int nc = cap_steps ? cap_steps * 2 : 64;
        step_t *ns = (step_t *)realloc(steps, sizeof(step_t) * (size_t)nc);
        if (!ns) return;
        steps = ns;
        cap_steps = nc;
    }
    step_t *s = &steps[nsteps++];
    s->op = op;
    s->src = src ? strdup(src) : 0;
    s->dst = dst ? strdup(dst) : 0;
    s->size = size;
    if (op == OP_COPY) job_total += size;
}

static void steps_clear(void) {
    for (int i = 0; i < nsteps; i++) { free(steps[i].src); free(steps[i].dst); }
    nsteps = at_step = 0;
}

/* The steps for copying `src` (a file or a whole folder) to `dst`. */
static void plan_copy(const char *src, const char *dst, int depth) {
    if (!is_dir(src)) {
        struct stat st;
        unsigned long long sz = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
        step_add(OP_COPY, src, dst, sz);
        return;
    }
    if (depth > 32) return;
    step_add(OP_MKDIR, 0, dst, 0);
    static struct xdirent buf[512];
    int n = readdir_x(src, buf, 512);
    if (n > 512) n = 512;
    /* the listing buffer is reused by the recursion: take the names first */
    char (*names)[256] = n > 0 ? malloc((size_t)n * 256) : 0;
    for (int i = 0; i < n && names; i++) snprintf(names[i], 256, "%s", buf[i].name);
    for (int i = 0; i < n && names; i++) {
        if (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0) continue;
        char s2[PATHMAX], d2[PATHMAX];
        path_join(s2, src, names[i]);
        path_join(d2, dst, names[i]);
        plan_copy(s2, d2, depth + 1);
    }
    free(names);
}

/* The steps for deleting `p`: what is inside a folder before the folder. */
static void plan_delete(const char *p, int depth) {
    if (!is_dir(p)) { step_add(OP_RMFILE, p, 0, 0); return; }
    if (depth > 32) return;
    static struct xdirent buf[512];
    int n = readdir_x(p, buf, 512);
    if (n > 512) n = 512;
    char (*names)[256] = n > 0 ? malloc((size_t)n * 256) : 0;
    for (int i = 0; i < n && names; i++) snprintf(names[i], 256, "%s", buf[i].name);
    for (int i = 0; i < n && names; i++) {
        if (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0) continue;
        char s2[PATHMAX];
        path_join(s2, p, names[i]);
        plan_delete(s2, depth + 1);
    }
    free(names);
    step_add(OP_RMDIR, p, 0, 0);
}

/* A name free in `dir` for `name`: "Отчёт.txt", "Отчёт (2).txt", ... */
static void free_name(char *out, const char *dir, const char *name, const char *suffix, int isdir) {
    char base[256], ext[64];
    snprintf(base, sizeof base, "%s", name);
    ext[0] = 0;
    char *d = strrchr(base, '.');
    if (d && d != base && !isdir) { snprintf(ext, sizeof ext, "%s", d); *d = 0; }
    for (int n = 1; n < 1000; n++) {
        char cand[300];
        if (n == 1 && suffix) snprintf(cand, sizeof cand, "%s%s%s", base, suffix, ext);
        else if (n == 1) snprintf(cand, sizeof cand, "%s%s", base, ext);
        else snprintf(cand, sizeof cand, "%s (%d)%s", base, n, ext);
        path_join(out, dir, cand);
        if (!exists(out)) return;
    }
}

static void job_start(int kind) {
    job = kind;
    job_done = 0;
    job_fail = 0;
    job_cancel = 0;
    at_step = 0;
    job_t0 = uptime_ms();
    snprintf(job_refresh, sizeof job_refresh, "%s", cwd);
    dirty = 1;
}

static void job_end(void) {
    if (cp_in >= 0) close(cp_in);
    if (cp_out >= 0) close(cp_out);
    cp_in = cp_out = -1;
    char msg[160];
    if (job_cancel) snprintf(msg, sizeof msg, "%s", T("Отменено", "Cancelled"));
    else if (job_fail) snprintf(msg, sizeof msg, T("Не удалось: %d", "Failed: %d"), job_fail);
    else snprintf(msg, sizeof msg, "%s", job == JOB_DELETE ? T("Удалено", "Deleted") :
                  job == JOB_MOVE ? T("Перемещено", "Moved") : T("Скопировано", "Copied"));
    set_status(msg);
    job = 0;
    steps_clear();
    job_total = 0;
    load();
    refresh_volumes();
    dirty = 1;
}

/* Do some of the job: about 30 ms of it, so the window keeps answering. */
static char iob[262144];
static void job_work(void) {
    if (!job) return;
    if (job_cancel) { job_end(); return; }
    unsigned long long t0 = uptime_ms();
    while (at_step < nsteps && uptime_ms() - t0 < 30) {
        step_t *s = &steps[at_step];
        switch (s->op) {
        case OP_MKDIR:
            if (!is_dir(s->dst) && mkdir(s->dst, 0755) != 0) job_fail++;
            at_step++;
            break;
        case OP_RMFILE:
            if (unlink(s->src) != 0) job_fail++;
            at_step++;
            break;
        case OP_RMDIR:
            if (rmdir(s->src) != 0 && unlink(s->src) != 0) job_fail++;
            at_step++;
            break;
        case OP_RENAME:
            if (rename(s->src, s->dst) != 0) job_fail++;
            at_step++;
            break;
        case OP_COPY:
            if (cp_in < 0) {
                cp_in = open(s->src, O_RDONLY);
                if (cp_in < 0) { job_fail++; at_step++; break; }
                cp_out = open(s->dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (cp_out < 0) { close(cp_in); cp_in = -1; job_fail++; at_step++; break; }
            }
            {
                ssize_t n = read(cp_in, iob, sizeof iob);
                if (n > 0) {
                    if (write(cp_out, iob, (size_t)n) != n) { job_fail++; n = 0; }
                    else job_done += (unsigned long long)n;
                }
                if (n <= 0) {
                    close(cp_in);
                    close(cp_out);
                    cp_in = cp_out = -1;
                    at_step++;
                }
            }
            break;
        }
    }
    dirty = 1;
    if (at_step >= nsteps) job_end();
}

/* --- what the user does to files -------------------------------------------------------- */

static void sel_paths(char *out, int max, const char *head) {
    int o = snprintf(out, max, "%s", head ? head : "");
    for (int i = 0; i < nview; i++) {
        item_t *it = &items[view[i]];
        if (!it->sel) continue;
        char full[PATHMAX];
        path_join(full, cwd, it->name);
        o += snprintf(out + o, (size_t)(max - o > 0 ? max - o : 0), "%s\n", full);
        if (o >= max - 1) break;
    }
}

static void do_copy(int cut) {
    if (!nsel()) return;
    static char buf[65536];
    sel_paths(buf, sizeof buf, cut ? "cut\n" : "copy\n");
    clip_set(CLIP_FILES, buf, (unsigned)strlen(buf));
    char m[96];
    snprintf(m, sizeof m, cut ? T("Вырезано: %d", "Cut: %d") : T("Скопировано в буфер: %d", "Copied: %d"), nsel());
    set_status(m);
}

/* Bring `paths` (one a line) into folder `dst`: moved if `move` and on the
 * same drive (a rename), copied otherwise -- and then, for a move, the
 * originals deleted. */
static void bring(const char *paths, const char *dst, int move) {
    if (job) return;
    steps_clear();
    job_total = 0;
    char line[PATHMAX];
    const char *p = paths;
    int any = 0;
    while (*p) {
        int n = 0;
        while (*p && *p != '\n' && n < PATHMAX - 1) line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n') p++;
        if (!n || !exists(line)) continue;
        char parent[PATHMAX];
        path_parent(parent, line);
        const char *base = path_base(line);
        char to[PATHMAX];
        if (move && strcmp(parent, dst) == 0) continue;            /* already here */
        /* a folder into itself or below itself: no */
        size_t ln = strlen(line);
        if (strncmp(dst, line, ln) == 0 && (dst[ln] == '/' || dst[ln] == 0)) {
            set_status(T("Папку нельзя переместить в неё саму", "A folder cannot go inside itself"));
            continue;
        }
        int isd = is_dir(line);
        if (strcmp(parent, dst) == 0) free_name(to, dst, base, T(" — копия", " copy"), isd);
        else {
            path_join(to, dst, base);
            if (exists(to)) free_name(to, dst, base, 0, isd);
        }
        if (move && same_drive(line, dst)) step_add(OP_RENAME, line, to, 0);
        else {
            plan_copy(line, to, 0);
            if (move) plan_delete(line, 0);
        }
        any = 1;
    }
    if (any) job_start(move ? JOB_MOVE : JOB_COPY);
}

static void do_paste(void) {
    static char buf[65536];
    int type, n = clip_get(buf, sizeof buf - 1, &type);
    if (n <= 0 || type != CLIP_FILES) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    buf[n] = 0;
    int cut = strncmp(buf, "cut\n", 4) == 0;
    const char *list = strchr(buf, '\n');
    if (!list) return;
    bring(list + 1, cwd, cut);
    if (cut) clip_set(CLIP_NONE, "", 0);
}

static int confirm;              /* a question is up: CONFIRM_* */
#define CONFIRM_DELETE 1
#define CONFIRM_OVERWRITE 2
static char confirm_text[300];

static void do_delete(void) {
    if (job || !nsel()) return;
    steps_clear();
    job_total = 0;
    for (int i = 0; i < nview; i++) {
        item_t *it = &items[view[i]];
        if (!it->sel || it->mount) continue;
        char full[PATHMAX];
        path_join(full, cwd, it->name);
        plan_delete(full, 0);
    }
    if (nsteps) job_start(JOB_DELETE);
}

/* Inline rename / a new folder's name. */
static ed_t ren;
static int ren_item = -1;           /* index into items, -1 when not renaming */
static int ren_new;                 /* it is a folder just made: Escape keeps the name */

static void begin_rename(int vi) {
    if (vi < 0 || vi >= nview || job) return;
    item_t *it = &items[view[vi]];
    if (it->mount || strcmp(cwd, "/") == 0) return;
    ren_item = view[vi];
    if (it->dir) ed_set(&ren, it->name, 1);
    else ed_set_name(&ren, it->name);
    /* the name without its extension is what is selected, as on Windows */
    dirty = 1;
}

static void end_rename(int commit) {
    if (ren_item < 0) return;
    item_t *it = &items[ren_item];
    if (commit && ren.s[0] && strcmp(ren.s, it->name) != 0) {
        if (strchr(ren.s, '/')) set_status(T("В имени не может быть «/»", "A name cannot contain '/'"));
        else {
            char from[PATHMAX], to[PATHMAX];
            path_join(from, cwd, it->name);
            path_join(to, cwd, ren.s);
            if (exists(to) && name_cmp(ren.s, it->name) != 0)
                set_status(T("Такое имя уже есть", "That name is taken"));
            else if (rename(from, to) != 0)
                set_status(T("Не удалось переименовать", "Could not rename"));
            else {
                char keep[256];
                snprintf(keep, sizeof keep, "%s", ren.s);
                load();
                clear_sel();
                for (int i = 0; i < nview; i++)
                    if (strcmp(items[view[i]].name, keep) == 0) { items[view[i]].sel = 1; cursor = anchor = i; }
            }
        }
    }
    ren_item = -1;
    ren.active = 0;
    ren_new = 0;
    dirty = 1;
}

static void new_folder(void) {
    if (job || strcmp(cwd, "/") == 0) return;
    char p[PATHMAX];
    free_name(p, cwd, T("Новая папка", "New folder"), 0, 1);
    if (mkdir(p, 0755) != 0) { set_status(T("Не удалось создать папку", "Could not create the folder")); return; }
    load();
    clear_sel();
    for (int i = 0; i < nview; i++)
        if (strcmp(items[view[i]].name, path_base(p)) == 0) {
            items[view[i]].sel = 1;
            cursor = anchor = i;
            begin_rename(i);
            ren_new = 1;
        }
}

static void new_text_file(void) {
    if (job || strcmp(cwd, "/") == 0) return;
    char p[PATHMAX];
    free_name(p, cwd, T("Новый документ.txt", "New document.txt"), 0, 0);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { set_status(T("Не удалось создать файл", "Could not create the file")); return; }
    close(fd);
    load();
    clear_sel();
    for (int i = 0; i < nview; i++)
        if (strcmp(items[view[i]].name, path_base(p)) == 0) {
            items[view[i]].sel = 1;
            cursor = anchor = i;
            begin_rename(i);
        }
}

/* --- opening ----------------------------------------------------------------------------- */

static const struct { const char *path, *ru, *en; } apps[] = {
    { "/bin/note", "Блокнот", "Notepad" },
    { "/bin/edit", "Редактор", "Editor" },
    { "/bin/view", "Просмотр", "Viewer" },
    { "/bin/play", "Музыка", "Music" },
    { "/bin/web",  "Браузер", "Browser" },
};
#define NAPPS ((int)(sizeof apps / sizeof apps[0]))

static const char *app_for(const item_t *it) {
    switch (it->kind) {
    case K_IMAGE: return "/bin/view";
    case K_AUDIO: return "/bin/play";
    case K_WEB:   return "/bin/web";
    case K_APP:   return 0;
    }
    if (ext_any(it->name, "sh,bat")) return "/bin/sh";
    return "/bin/note";
}

static void pick_finish(const char *result);

static void open_item(int vi, const char *with) {
    if (vi < 0 || vi >= nview) return;
    item_t *it = &items[view[vi]];
    char full[PATHMAX];
    path_join(full, cwd, it->name);
    if (it->dir) { go(full, 1); return; }
    if (pick == PICK_OPEN) { pick_finish(full); return; }
    if (pick == PICK_SAVE) return;
    const char *app = with ? with : app_for(it);
    int r = app ? spawn_window(app, full) : spawn_window(full, 0);
    if (r < 0) set_status(T("Не удалось открыть", "Could not open it"));
}

/* --- the picker's answer -------------------------------------------------------------------- */

static ed_t pick_name;
static int running = 1;

static void pick_finish(const char *result) {
    /* Written aside and then renamed into place: the program waiting for the
     * answer never reads half of one. */
    char out[PATHMAX + 8], tmp[PATHMAX + 8];
    snprintf(out, sizeof out, "%s.out", pick_req);
    snprintf(tmp, sizeof tmp, "%s.tmp", pick_req);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (result) write(fd, result, strlen(result));
        close(fd);
        rename(tmp, out);
    }
    running = 0;
}

static char pending_save[PATHMAX];

static void pick_accept(void) {
    if (pick == PICK_DIR) { pick_finish(cwd); return; }
    if (pick == PICK_OPEN) {
        for (int i = 0; i < nview; i++) {
            item_t *it = &items[view[i]];
            if (it->sel && !it->dir) { open_item(i, 0); return; }
        }
        if (cursor >= 0 && cursor < nview) open_item(cursor, 0);
        return;
    }
    /* save */
    if (!pick_name.s[0] || strcmp(cwd, "/") == 0) return;
    char name[256];
    snprintf(name, sizeof name, "%s", pick_name.s);
    if (strchr(name, '/')) { set_status(T("В имени не может быть «/»", "A name cannot contain '/'")); return; }
    if (pick_exts[0] && !strchr(name, '.')) {
        char e[16];
        int n = 0;
        while (pick_exts[n] && pick_exts[n] != ',' && n < 15) { e[n] = pick_exts[n]; n++; }
        e[n] = 0;
        size_t l = strlen(name);
        snprintf(name + l, sizeof name - l, ".%s", e);
    }
    char full[PATHMAX];
    path_join(full, cwd, name);
    if (is_dir(full)) { go(full, 1); return; }
    if (exists(full)) {
        snprintf(pending_save, sizeof pending_save, "%s", full);
        snprintf(confirm_text, sizeof confirm_text,
                 T("Файл «%s» уже есть. Заменить его?", "\"%s\" already exists. Replace it?"), name);
        confirm = CONFIRM_OVERWRITE;
        dirty = 1;
        return;
    }
    pick_finish(full);
}

/* --- layout -------------------------------------------------------------------------------- */

#define SIDE_W   236
#define TOP_H    58
#define STATUS_H 30
#define PICK_H   92
#define HEAD_H   32
#define ROW_H    34
#define TILE_W   118
#define TILE_H   118

static int content_x(void) { return SIDE_W; }
static int content_y(void) { return TOP_H + (grid ? 0 : HEAD_H); }
static int content_w(void) { return g.w - SIDE_W; }
static int bottom_h(void) { return STATUS_H + (pick ? PICK_H : 0); }
static int content_h(void) { return g.h - content_y() - bottom_h(); }

static int grid_cols(void) {
    int c = (content_w() - 24) / TILE_W;
    return c < 1 ? 1 : c;
}
static int total_h(void) {
    if (grid) return ((nview + grid_cols() - 1) / grid_cols()) * TILE_H + 24;
    return nview * ROW_H + 8;
}
static void clamp_scroll(void) {
    int max = total_h() - content_h();
    if (scroll_y > max) scroll_y = max;
    if (scroll_y < 0) scroll_y = 0;
}

/* Where item vi is drawn. */
static void item_rect(int vi, int *x, int *y, int *w, int *h) {
    if (grid) {
        int c = grid_cols();
        int pad = (content_w() - c * TILE_W) / 2;
        *x = content_x() + pad + (vi % c) * TILE_W;
        *y = content_y() + 12 + (vi / c) * TILE_H - scroll_y;
        *w = TILE_W;
        *h = TILE_H;
    } else {
        *x = content_x() + 8;
        *y = content_y() + 4 + vi * ROW_H - scroll_y;
        *w = content_w() - 16;
        *h = ROW_H;
    }
}

static int item_at(int mx, int my) {
    if (mx < content_x() || my < content_y() || my >= content_y() + content_h()) return -1;
    for (int i = 0; i < nview; i++) {
        int x, y, w, h;
        item_rect(i, &x, &y, &w, &h);
        if (grid) { x += 6; y += 4; w -= 12; h -= 8; }
        if (mx >= x && mx < x + w && my >= y && my < y + h) return i;
    }
    return -1;
}

static void scroll_to(int vi) {
    if (vi < 0) return;
    int x, y, w, h;
    item_rect(vi, &x, &y, &w, &h);
    int top = content_y(), bot = content_y() + content_h();
    if (y < top) scroll_y -= top - y + 4;
    else if (y + h > bot) scroll_y += y + h - bot + 4;
    clamp_scroll();
}

/* list columns: name | modified | size | type */
static void cols(int *xn, int *xd, int *xs, int *xt, int *xe) {
    int x0 = content_x() + 8, x1 = content_x() + content_w() - 16;
    int wt = 130, ws = 96, wd = 150;
    if (x1 - x0 < 640) { wt = 0; }
    *xe = x1;
    *xt = x1 - wt;
    *xs = *xt - ws;
    *xd = *xs - wd;
    *xn = x0;
}

/* --- side bar ---------------------------------------------------------------------------- */

typedef struct { int kind; int idx; int y, h; } side_row;     /* kind 0 place, 1 volume */
static side_row srows[32];
static int nsrows;
static int side_hot = -1, side_eject_hot = -1;

static void side_layout(void) {
    nsrows = 0;
    int y = 16 + 28;
    for (int i = 0; i < NSTD; i++) {
        if (!is_dir(stdf[i].path)) continue;
        srows[nsrows++] = (side_row){ 0, i, y, 34 };
        y += 34;
    }
    y += 16 + 28;
    for (int i = 0; i < nvols && nsrows < 32; i++) {
        srows[nsrows++] = (side_row){ 1, i, y, 50 };
        y += 50;
    }
}
static int side_heading_y(int which) {   /* 0 places, 1 drives */
    if (!which) return 16;
    int y = 16 + 28;
    for (int i = 0; i < NSTD; i++) if (is_dir(stdf[i].path)) y += 34;
    return y + 16;
}

static int side_at(int mx, int my) {
    if (mx >= SIDE_W) return -1;
    for (int i = 0; i < nsrows; i++) if (my >= srows[i].y && my < srows[i].y + srows[i].h) return i;
    return -1;
}

static void eject_rect(const side_row *r, int *x, int *y, int *w, int *h) {
    *w = 26; *h = 26;
    *x = SIDE_W - 14 - 26;
    *y = r->y + (r->h - 26) / 2 - 5;
}

static void draw_side(void) {
    rrect(0, 0, SIDE_W, g.h, 0, GC_BAR, 255);
    rrect(SIDE_W - 1, 0, 1, g.h, 0, GC_LINE, 255);
    side_layout();
    unsigned dim = GC_DIM;
    td(&F_HEAD, 22, side_heading_y(0), 24, T("Избранное", "Places"), dim);
    td(&F_HEAD, 22, side_heading_y(1), 24, T("Устройства", "Drives"), dim);
    for (int i = 0; i < nsrows; i++) {
        side_row *r = &srows[i];
        int x = 10, w = SIDE_W - 20;
        int cur = 0;
        if (r->kind == 0) cur = strcmp(cwd, stdf[r->idx].path) == 0;
        else {
            const char *m = vols[r->idx].mount;
            cur = strcmp(cwd, m) == 0;
        }
        if (cur) rrect(x, r->y + 1, w, r->h - 2, 9, GC_ACCENT, 46);
        else if (i == side_hot) rrect(x, r->y + 1, w, r->h - 2, 9, GC_TEXT, 16);
        if (r->kind == 0) {
            glyph_place(stdf[r->idx].glyph, x + 12, r->y + 8, 18, cur ? GC_ACCENT : mix(GC_ACCENT, GC_DIM, 90));
            td_fit(&F_UI, x + 42, r->y, r->h, en ? stdf[r->idx].en : stdf[r->idx].ru, w - 50,
                   cur ? GC_ACCENT2 : GC_TEXT);
        } else {
            const struct uvol *v = &vols[r->idx];
            draw_icon(v->index < 0 ? K_DRIVE : K_USB, x + 6, r->y + 6, 28, "");
            int ejw = v->index >= 0 ? 30 : 0;
            td_fit(&F_UI, x + 42, r->y + 4, 22, vol_title(v), w - 50 - ejw, cur ? GC_ACCENT2 : GC_TEXT);
            /* how full it is */
            int bx = x + 42, by = r->y + 31, bw = w - 52 - ejw;
            rrect(bx, by, bw, 5, 3, GC_LINE, 255);
            if (v->total) {
                unsigned long long used = v->total > v->free ? v->total - v->free : 0;
                int fw = (int)((unsigned long long)bw * used / v->total);
                unsigned c = used * 10 > v->total * 9 ? GC_WARN : GC_ACCENT;
                if (v->free || v->index < 0) rrect(bx, by, fw < 4 ? 4 : fw, 5, 3, c, 255);
            }
            char fr[48], line[96];
            fmt_size(fr, sizeof fr, v->free);
            snprintf(line, sizeof line, T("свободно %s", "%s free"), fr);
            if (!v->usable) snprintf(line, sizeof line, "%s · %s", v->fs, T("не читается", "not readable"));
            td_fit(&F_SMALL, bx, r->y + 36, 16, line, bw + ejw, GC_DIM);
            if (v->index >= 0) {
                int ex, ey, ew, eh;
                eject_rect(r, &ex, &ey, &ew, &eh);
                if (i == side_eject_hot) rrect(ex, ey, ew, eh, 8, GC_TEXT, 26);
                unsigned c = i == side_eject_hot ? GC_TEXT : GC_DIM;
                tri((float)(ex + 7), (float)(ey + 15), (float)(ex + 13), (float)(ey + 8), (float)(ex + 19), (float)(ey + 15), c, 255);
                rrect(ex + 7, ey + 17, 12, 2, 1, c, 255);
            }
        }
    }
}

/* --- tool bar ------------------------------------------------------------------------------ */

#define TB_BACK 0
#define TB_FWD  1
#define TB_UP   2
#define TB_GRID 3
#define TB_NEW  4
static int tb_hot = -1;
static ed_t srch;
static ed_t pathed;             /* the path, typed (Ctrl+L) */
static int focus_field;         /* 0 the files, 1 search, 2 path, 3 the picker's name */

static void tb_rect(int which, int *x, int *y, int *w, int *h) {
    *y = 12; *h = 34; *w = 34;
    switch (which) {
    case TB_BACK: *x = SIDE_W + 12; break;
    case TB_FWD:  *x = SIDE_W + 48; break;
    case TB_UP:   *x = SIDE_W + 84; break;
    case TB_GRID: *x = g.w - 16 - 34; break;
    case TB_NEW:  *x = g.w - 16 - 34 - 40; break;
    }
}
static void search_rect(int *x, int *y, int *w, int *h) {
    *w = g.w - SIDE_W > 900 ? 240 : 180;
    *h = 34;
    *y = 12;
    *x = g.w - 16 - 34 - 40 - 12 - *w;
}
static void crumbs_rect(int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh;
    search_rect(&sx, &sy, &sw, &sh);
    *x = SIDE_W + 128;
    *y = 12;
    *w = sx - 12 - *x;
    *h = 34;
}

/* The breadcrumb's pieces: where each one goes and where it is drawn. */
typedef struct { char path[PATHMAX]; char name[256]; int x, w; } crumb_t;
static crumb_t crumbs[24];
static int ncrumbs, crumb_hot = -1;

static void crumbs_layout(void) {
    int cx, cy, cw, ch;
    crumbs_rect(&cx, &cy, &cw, &ch);
    ncrumbs = 0;
    /* from the drive (or the system) down to cwd */
    char acc[PATHMAX];
    const struct uvol *v = vol_for(cwd);
    char drive[32];
    drive_of(cwd, drive);
    int home = strncmp(cwd, "/home", 5) == 0 && (cwd[5] == 0 || cwd[5] == '/');
    if (home) strcpy(acc, "/home");
    else strcpy(acc, drive);
    crumb_t *c = &crumbs[ncrumbs++];
    snprintf(c->path, PATHMAX, "%s", acc);
    snprintf(c->name, sizeof c->name, "%s", home ? T("Домашняя папка", "Home") :
             v ? vol_title(v) : T("Система", "System"));
    const char *rest = cwd + strlen(acc);
    while (*rest && ncrumbs < 24) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *e = rest;
        while (*e && *e != '/') e++;
        size_t l = strlen(acc);
        if (l > 1) acc[l++] = '/';
        else if (acc[0] == '/' && acc[1] == 0) l = 1;
        memcpy(acc + l, rest, (size_t)(e - rest));
        acc[l + (size_t)(e - rest)] = 0;
        c = &crumbs[ncrumbs++];
        snprintf(c->path, PATHMAX, "%s", acc);
        char nm[256];
        snprintf(nm, sizeof nm, "%.*s", (int)(e - rest), rest);
        snprintf(c->name, sizeof c->name, "%s", disp_name(acc, nm));
        rest = e;
    }
    /* widths; the first ones give way when it does not fit */
    int sep = 22, total = 0;
    for (int i = 0; i < ncrumbs; i++) {
        crumbs[i].w = tw(i == ncrumbs - 1 ? &F_BOLD : &F_UI, crumbs[i].name) + 16;
        if (crumbs[i].w > 220) crumbs[i].w = 220;
        total += crumbs[i].w + (i ? sep : 0);
    }
    int first = 0;
    while (total > cw - 10 && first < ncrumbs - 1) { total -= crumbs[first].w + sep; first++; }
    int x = cx + 4;
    for (int i = 0; i < ncrumbs; i++) {
        if (i < first) { crumbs[i].w = 0; crumbs[i].x = -1000; continue; }
        crumbs[i].x = x;
        x += crumbs[i].w + sep;
    }
}

static void tb_button(int which, int hot) {
    int x, y, w, h;
    tb_rect(which, &x, &y, &w, &h);
    int enabled = which == TB_BACK ? hist_at > 0 : which == TB_FWD ? hist_at < hist_n - 1 :
                  which == TB_UP ? strcmp(cwd, "/") != 0 : 1;
    if (which == TB_NEW && (pick == PICK_OPEN || strcmp(cwd, "/") == 0)) enabled = 0;
    if (hot && enabled) rrect(x, y, w, h, 9, GC_TEXT, 22);
    unsigned c = enabled ? GC_TEXT : mix(GC_DIM, GC_WIN, 120);
    float cx = x + w / 2.f, cy = y + h / 2.f;
    switch (which) {
    case TB_BACK:
        tri(cx - 6, cy, cx + 3, cy - 7, cx + 3, cy + 7, c, 255);
        break;
    case TB_FWD:
        tri(cx + 6, cy, cx - 3, cy - 7, cx - 3, cy + 7, c, 255);
        break;
    case TB_UP:
        tri(cx, cy - 7, cx - 7, cy + 2, cx + 7, cy + 2, c, 255);
        rrect((int)cx - 2, (int)cy + 1, 4, 7, 1, c, 255);
        break;
    case TB_GRID:
        if (grid) {
            for (int i = 0; i < 3; i++) rrect((int)cx - 8, (int)cy - 7 + i * 6, 16, 3, 1, c, 255);
        } else {
            for (int i = 0; i < 4; i++) rrect((int)cx - 8 + (i % 2) * 9, (int)cy - 8 + (i / 2) * 9, 7, 7, 2, c, 255);
        }
        break;
    case TB_NEW:
        icon_folder(x + 6, y + 5, 22, mix(GC_ACCENT, 0x5AB0F0, 120));
        rrect(x + 21, y + 17, 10, 10, 5, GC_GOOD, 255);
        rrect(x + 23, y + 21, 6, 2, 1, 0xFFFFFF, 255);
        rrect(x + 25, y + 19, 2, 6, 1, 0xFFFFFF, 255);
        break;
    }
}

static void draw_toolbar(int blink) {
    rrect(SIDE_W, 0, g.w - SIDE_W, TOP_H, 0, GC_WIN, 255);
    rrect(SIDE_W, TOP_H - 1, g.w - SIDE_W, 1, 0, GC_LINE, 255);
    for (int b = TB_BACK; b <= TB_NEW; b++) tb_button(b, tb_hot == b);
    int cx, cy, cw, ch;
    crumbs_rect(&cx, &cy, &cw, &ch);
    if (focus_field == 2) {
        ed_draw(&pathed, cx, cy, cw, ch, &F_UI, 1, blink);
    } else {
        rrect(cx, cy, cw, ch, 9, GC_PANEL, 255);
        rline(cx, cy, cw, ch, 9, GC_EDGE, 120);
        crumbs_layout();
        clip_to(cx + 2, cy, cw - 4, ch);
        for (int i = 0; i < ncrumbs; i++) {
            crumb_t *c = &crumbs[i];
            if (c->x < 0) continue;
            int last = i == ncrumbs - 1;
            if (i == crumb_hot && !last) rrect(c->x, cy + 4, c->w, ch - 8, 6, GC_TEXT, 18);
            td_fit(last ? &F_BOLD : &F_UI, c->x + 8, cy, ch, c->name, c->w - 14, last ? GC_TEXT : GC_DIM);
            if (!last) {
                float ax = (float)(c->x + c->w + 8), ay = cy + ch / 2.f;
                tri(ax, ay - 4, ax + 5, ay, ax, ay + 4, GC_DIM, 200);
            }
        }
        clip_all();
    }
    int sx, sy, sw, sh;
    search_rect(&sx, &sy, &sw, &sh);
    if (focus_field == 1 || srch.s[0]) ed_draw(&srch, sx + 0, sy, sw, sh, &F_UI, focus_field == 1, blink);
    else {
        rrect(sx, sy, sw, sh, 9, GC_PANEL, 255);
        rline(sx, sy, sw, sh, 9, GC_EDGE, 120);
        td(&F_UI, sx + 34, sy, sh, T("Поиск", "Search"), GC_DIM);
    }
    /* the magnifier */
    if (!(focus_field == 1 || srch.s[0])) {
        disc((float)(sx + 17), (float)(sy + 15), 6.f, GC_DIM, 255);
        disc((float)(sx + 17), (float)(sy + 15), 4.2f, GC_PANEL, 255);
        tri((float)(sx + 20), (float)(sy + 19), (float)(sx + 22), (float)(sy + 17), (float)(sx + 27), (float)(sy + 25), GC_DIM, 255);
    }
}

/* --- the files --------------------------------------------------------------------------- */

static int hot = -1;                 /* view index under the pointer */
static int drop_vi = -1;             /* a folder something would be dropped into */
static int drop_here;                /* ... or the folder on show */
static int band, band_x0, band_y0, band_x1, band_y1;   /* rubber band */

static void fmt_date(char *out, int max, long long t) {
    if (t <= 0) { snprintf(out, max, "—"); return; }
    long long days = t / 86400, sec = t % 86400;
    /* civil from days */
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, max, "%02u.%02u.%lld %02d:%02d", d, m, y, (int)(sec / 3600), (int)(sec / 60 % 60));
}

static void draw_files(int blink) {
    int x0 = content_x(), y0 = TOP_H, w = content_w(), h = g.h - TOP_H - bottom_h();
    rrect(x0, y0, w, h, 0, GC_PANEL, 255);
    if (drop_here) rrect(x0 + 4, y0 + 4, w - 8, h - 8, 12, GC_ACCENT, 26);
    int xn, xd, xs, xt, xe;
    cols(&xn, &xd, &xs, &xt, &xe);
    if (!grid) {
        rrect(x0, y0, w, HEAD_H, 0, GC_PANEL, 255);
        static const char *ru_h[] = { "Имя", "Изменён", "Размер", "Тип" }, *en_h[] = { "Name", "Modified", "Size", "Type" };
        int hx[4] = { xn + 40, xd, xs, xt };
        for (int c = 0; c < 4; c++) {
            if (c == 3 && xt == xe) continue;
            const char *t = en ? en_h[c] : ru_h[c];
            int tx = c == 2 ? xt - 12 - tw(&F_HEAD, t) : hx[c];
            td(&F_HEAD, tx, y0, HEAD_H, t, sort_col == (c == 0 ? 0 : c == 1 ? 1 : c == 2 ? 2 : 3) ? GC_TEXT : GC_DIM);
            if (sort_col == c) {
                float ax = (float)(tx + tw(&F_HEAD, t) + 7), ay = y0 + HEAD_H / 2.f;
                if (sort_desc) tri(ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3, GC_DIM, 255);
                else tri(ax - 4, ay + 2, ax + 4, ay + 2, ax, ay - 3, GC_DIM, 255);
            }
        }
        rrect(x0 + 12, y0 + HEAD_H - 1, w - 24, 1, 0, GC_LINE, 255);
    }
    clip_to(x0, content_y(), w, content_h());
    if (!nview) {
        const char *msg = search[0] ? T("Ничего не найдено", "Nothing found") :
                          T("Папка пуста", "This folder is empty");
        td(&F_UI, x0 + (w - tw(&F_UI, msg)) / 2, content_y() + 40, 30, msg, GC_DIM);
    }
    for (int vi = 0; vi < nview; vi++) {
        int x, y, iw, ih;
        item_rect(vi, &x, &y, &iw, &ih);
        if (y + ih < content_y() || y > content_y() + content_h()) continue;
        item_t *it = &items[view[vi]];
        int sel = it->sel, isren = ren_item == view[vi];
        int fade = job == JOB_MOVE ? 0 : 0;
        (void)fade;
        if (grid) {
            int bx = x + 6, by = y + 4, bw = iw - 12, bh = ih - 8;
            if (sel) rrect(bx, by, bw, bh, 12, GC_ACCENT, 50);
            else if (vi == hot) rrect(bx, by, bw, bh, 12, GC_TEXT, 14);
            if (vi == drop_vi) rline(bx, by, bw, bh, 12, GC_ACCENT, 255);
            if (vi == cursor && !sel && focus_field == 0) rline(bx, by, bw, bh, 12, GC_ACCENT, 120);
            draw_icon(it->kind, x + (iw - 60) / 2, y + 10, 60, it->name);
            if (isren) ed_draw(&ren, x + 4, y + 74, iw - 8, 30, &F_UI, 1, blink);
            else {
                /* the name, on two lines when it needs them */
                const char *s = it->disp;
                int maxw = iw - 14;
                size_t len = strlen(s);
                if (tw(&F_UI, s) <= maxw || !have_txt) {
                    td_fit(&F_UI, x + (iw - (tw(&F_UI, s) < maxw ? tw(&F_UI, s) : maxw)) / 2, y + 74, 20, s, maxw, GC_TEXT);
                } else {
                    int at = 0;
                    size_t brk = txt_split(&F_UI, s, len, maxw, &at);
                    char l1[256];
                    snprintf(l1, sizeof l1, "%.*s", (int)brk, s);
                    const char *rest = s + brk;
                    while (*rest == ' ') rest++;
                    int w1 = tw(&F_UI, l1);
                    td_fit(&F_UI, x + (iw - (w1 < maxw ? w1 : maxw)) / 2, y + 72, 20, l1, maxw, GC_TEXT);
                    int w2 = tw(&F_UI, rest);
                    td_fit(&F_UI, x + (iw - (w2 < maxw ? w2 : maxw)) / 2, y + 91, 20, rest, maxw, GC_TEXT);
                }
            }
        } else {
            if (sel) rrect(x, y + 1, iw, ih - 2, 8, GC_ACCENT, 46);
            else if (vi == hot) rrect(x, y + 1, iw, ih - 2, 8, GC_TEXT, 12);
            if (vi == drop_vi) rline(x, y + 1, iw, ih - 2, 8, GC_ACCENT, 255);
            if (vi == cursor && !sel && focus_field == 0) rline(x, y + 1, iw, ih - 2, 8, GC_ACCENT, 110);
            draw_icon(it->kind, xn + 8, y + 5, 24, it->name);
            if (isren) ed_draw(&ren, xn + 38, y + 3, xd - xn - 50, ih - 6, &F_UI, 1, blink);
            else td_fit(&F_UI, xn + 42, y, ih, it->disp, xd - xn - 56, GC_TEXT);
            char buf[64];
            fmt_date(buf, sizeof buf, it->mtime);
            if (it->mount) buf[0] = 0;
            td(&F_SMALL, xd, y, ih, buf, GC_DIM);
            if (!it->dir) {
                fmt_size(buf, sizeof buf, it->size);
                td(&F_SMALL, xt - 12 - tw(&F_SMALL, buf), y, ih, buf, GC_DIM);
            }
            if (xt != xe) td_fit(&F_SMALL, xt, y, ih, kind_name(it->kind, it->name), xe - xt - 4, GC_DIM);
        }
    }
    if (band) {
        int bx = band_x0 < band_x1 ? band_x0 : band_x1, by = band_y0 < band_y1 ? band_y0 : band_y1;
        int bw = abs(band_x1 - band_x0), bh = abs(band_y1 - band_y0);
        rrect(bx, by, bw, bh, 3, GC_ACCENT, 40);
        rline(bx, by, bw, bh, 3, GC_ACCENT, 200);
    }
    /* a thin scroll bar, when there is more than fits */
    int th = total_h(), vh = content_h();
    if (th > vh) {
        int bh = vh * vh / th;
        if (bh < 30) bh = 30;
        int by = content_y() + (vh - bh) * scroll_y / (th - vh);
        rrect(g.w - 8, by, 5, bh, 3, GC_TEXT, 70);
    }
    clip_all();
}

/* --- the bottom: status, and the picker's bar ------------------------------------------- */

#define PB_CANCEL 0
#define PB_OK     1
static int pb_hot = -1;

static void pb_rect(int which, int *x, int *y, int *w, int *h) {
    *h = 36; *w = 130;
    *y = g.h - PICK_H + 40;
    *x = which == PB_OK ? g.w - 16 - 130 : g.w - 16 - 130 - 10 - 130;
}
static void pname_rect(int *x, int *y, int *w, int *h) {
    int bx, by, bw, bh;
    pb_rect(PB_CANCEL, &bx, &by, &bw, &bh);
    *x = SIDE_W + 16 + 64;
    *y = by;
    *w = bx - 16 - *x;
    *h = 36;
}

static void button(int x, int y, int w, int h, const char *label, int primary, int hot_, int enabled) {
    if (primary) {
        rrect(x, y, w, h, 10, enabled ? GC_ACCENT : mix(GC_ACCENT, GC_WIN, 140), 255);
        if (hot_ && enabled) rrect(x, y, w, h, 10, 0xFFFFFF, 30);
        td(&F_BOLD, x + (w - tw(&F_BOLD, label)) / 2, y, h, label, 0xFFFFFF);
    } else {
        rrect(x, y, w, h, 10, GC_BTN, 255);
        rline(x, y, w, h, 10, GC_EDGE, 140);
        if (hot_) rrect(x, y, w, h, 10, GC_TEXT, 16);
        td(&F_UI, x + (w - tw(&F_UI, label)) / 2, y, h, label, GC_TEXT);
    }
}

static void draw_bottom(int blink) {
    int y = g.h - bottom_h();
    rrect(SIDE_W, y, g.w - SIDE_W, STATUS_H, 0, GC_WIN, 255);
    rrect(SIDE_W, y, g.w - SIDE_W, 1, 0, GC_LINE, 255);
    char s[200];
    int ns = nsel();
    if (status_msg[0] && uptime_ms() < status_until) snprintf(s, sizeof s, "%s", status_msg);
    else if (ns) {
        unsigned long long b = 0;
        for (int i = 0; i < nitems; i++) if (items[i].sel && !items[i].dir) b += items[i].size;
        char sz[32];
        fmt_size(sz, sizeof sz, b);
        snprintf(s, sizeof s, T("Выбрано %d из %d · %s", "%d of %d selected · %s"), ns, nview, sz);
    } else snprintf(s, sizeof s, T("Объектов: %d", "%d items"), nview);
    td(&F_SMALL, SIDE_W + 16, y, STATUS_H, s, GC_DIM);
    const struct uvol *v = vol_for(cwd);
    if (!v && nvols) v = &vols[0];
    if (v) {
        char fr[32], tot[32], line[96];
        fmt_size(fr, sizeof fr, v->free);
        fmt_size(tot, sizeof tot, v->total);
        snprintf(line, sizeof line, T("Свободно %s из %s", "%s free of %s"), fr, tot);
        td(&F_SMALL, g.w - 16 - tw(&F_SMALL, line), y, STATUS_H, line, GC_DIM);
    }
    if (!pick) return;
    int py = g.h - PICK_H;
    rrect(SIDE_W, py, g.w - SIDE_W, PICK_H, 0, GC_BAR, 255);
    rrect(SIDE_W, py, g.w - SIDE_W, 1, 0, GC_LINE, 255);
    int bx, by, bw, bh;
    {
        const char *t = pick_title[0] ? pick_title : pick == PICK_DIR ? T("Выберите папку", "Choose a folder") :
                        pick == PICK_SAVE ? T("Сохранить", "Save") : T("Выберите файл", "Choose a file");
        td(&F_BOLD, SIDE_W + 16, py + 8, 26, t, GC_TEXT);
        if (pick_label[0]) td(&F_SMALL, SIDE_W + 24 + tw(&F_BOLD, t), py + 8, 26, pick_label, GC_DIM);
    }
    if (pick == PICK_SAVE) {
        int nx, ny, nw, nh;
        pname_rect(&nx, &ny, &nw, &nh);
        td(&F_UI, SIDE_W + 16, ny, nh, T("Имя:", "Name:"), GC_DIM);
        ed_draw(&pick_name, nx, ny, nw, nh, &F_UI, focus_field == 3, blink);
    } else if (pick == PICK_OPEN) {
        /* what will be opened */
        for (int i = 0; i < nview; i++) {
            item_t *it = &items[view[i]];
            if (!it->sel || it->dir) continue;
            int bx0, by0, bw0, bh0;
            pb_rect(PB_CANCEL, &bx0, &by0, &bw0, &bh0);
            td_fit(&F_UI, SIDE_W + 16, by0, bh0, it->disp, bx0 - SIDE_W - 32, GC_TEXT);
            break;
        }
    }
    int ok_en = pick == PICK_SAVE ? pick_name.s[0] && strcmp(cwd, "/") != 0 : 1;
    if (pick == PICK_OPEN) {
        ok_en = 0;
        for (int i = 0; i < nitems; i++) if (items[i].sel && !items[i].dir) ok_en = 1;
    }
    pb_rect(PB_CANCEL, &bx, &by, &bw, &bh);
    button(bx, by, bw, bh, T("Отмена", "Cancel"), 0, pb_hot == PB_CANCEL, 1);
    pb_rect(PB_OK, &bx, &by, &bw, &bh);
    button(bx, by, bw, bh, pick == PICK_SAVE ? T("Сохранить", "Save") : pick == PICK_DIR ? T("Выбрать", "Choose")
                                             : T("Открыть", "Open"), 1, pb_hot == PB_OK, ok_en);
}

/* --- the menu ------------------------------------------------------------------------------ */

#define MI_OPEN 1
#define MI_OPENWITH 2
#define MI_CUT 3
#define MI_COPY 4
#define MI_PASTE 5
#define MI_RENAME 6
#define MI_DELETE 7
#define MI_PROPS 8
#define MI_NEWDIR 9
#define MI_NEWFILE 10
#define MI_VIEW 11
#define MI_HIDDEN 12
#define MI_REFRESH 13
#define MI_EJECT 14
#define MI_APP 100            /* + index into apps[] */
#define MI_SEP -1

static int menu_open_, menu_x, menu_y, menu_hot = -1;
static int menu_items[24], menu_n;
static int sub_open = -1;            /* index in menu_items of the item whose submenu shows */
static int sub_hot = -1;
static int menu_side = -1;           /* the menu is for a side-bar row */

static const char *menu_label(int id) {
    if (id >= MI_APP) return en ? apps[id - MI_APP].en : apps[id - MI_APP].ru;
    switch (id) {
    case MI_OPEN: return T("Открыть", "Open");
    case MI_OPENWITH: return T("Открыть с помощью", "Open with");
    case MI_CUT: return T("Вырезать", "Cut");
    case MI_COPY: return T("Копировать", "Copy");
    case MI_PASTE: return T("Вставить", "Paste");
    case MI_RENAME: return T("Переименовать", "Rename");
    case MI_DELETE: return T("Удалить", "Delete");
    case MI_PROPS: return T("Свойства", "Properties");
    case MI_NEWDIR: return T("Новая папка", "New folder");
    case MI_NEWFILE: return T("Новый текстовый документ", "New text document");
    case MI_VIEW: return grid ? T("Вид: список", "View as list") : T("Вид: значки", "View as icons");
    case MI_HIDDEN: return show_hidden ? T("Скрыть скрытые файлы", "Hide hidden files")
                                       : T("Показать скрытые файлы", "Show hidden files");
    case MI_REFRESH: return T("Обновить", "Refresh");
    case MI_EJECT: return T("Извлечь", "Eject");
    }
    return "";
}
static const char *menu_key(int id) {
    switch (id) {
    case MI_CUT: return "Ctrl+X";
    case MI_COPY: return "Ctrl+C";
    case MI_PASTE: return "Ctrl+V";
    case MI_RENAME: return "F2";
    case MI_DELETE: return "Del";
    case MI_NEWDIR: return "Ctrl+Shift+N";
    case MI_REFRESH: return "F5";
    }
    return "";
}

static int clip_has_files(void) { int t; clip_seq(&t); return t == CLIP_FILES; }

static void menu_show(int x, int y, int on_item) {
    menu_n = 0;
    int writable = strcmp(cwd, "/") != 0 && !pick;
    if (menu_side >= 0) {
        menu_items[menu_n++] = MI_OPEN;
        if (srows[menu_side].kind == 1 && vols[srows[menu_side].idx].index >= 0) menu_items[menu_n++] = MI_EJECT;
        menu_items[menu_n++] = MI_PROPS;
    } else if (on_item) {
        int one_file = 0, mounts = 0;
        for (int i = 0; i < nitems; i++) if (items[i].sel) { if (!items[i].dir) one_file++; if (items[i].mount) mounts++; }
        menu_items[menu_n++] = MI_OPEN;
        if (one_file == 1 && nsel() == 1 && !pick) menu_items[menu_n++] = MI_OPENWITH;
        if (!pick) {
            menu_items[menu_n++] = MI_SEP;
            if (writable && !mounts) menu_items[menu_n++] = MI_CUT;
            if (!mounts) menu_items[menu_n++] = MI_COPY;
            if (writable && clip_has_files()) menu_items[menu_n++] = MI_PASTE;
            menu_items[menu_n++] = MI_SEP;
            if (writable && nsel() == 1 && !mounts) menu_items[menu_n++] = MI_RENAME;
            if (writable && !mounts) menu_items[menu_n++] = MI_DELETE;
        }
        menu_items[menu_n++] = MI_SEP;
        menu_items[menu_n++] = MI_PROPS;
    } else {
        if (writable && clip_has_files()) { menu_items[menu_n++] = MI_PASTE; menu_items[menu_n++] = MI_SEP; }
        if (strcmp(cwd, "/") != 0 && pick != PICK_OPEN) {
            menu_items[menu_n++] = MI_NEWDIR;
            if (!pick) menu_items[menu_n++] = MI_NEWFILE;
            menu_items[menu_n++] = MI_SEP;
        }
        menu_items[menu_n++] = MI_VIEW;
        menu_items[menu_n++] = MI_HIDDEN;
        menu_items[menu_n++] = MI_REFRESH;
        menu_items[menu_n++] = MI_SEP;
        menu_items[menu_n++] = MI_PROPS;
    }
    /* no separator first, last or twice */
    int o = 0;
    for (int i = 0; i < menu_n; i++) {
        if (menu_items[i] == MI_SEP && (o == 0 || menu_items[o - 1] == MI_SEP)) continue;
        menu_items[o++] = menu_items[i];
    }
    while (o && menu_items[o - 1] == MI_SEP) o--;
    menu_n = o;
    menu_x = x;
    menu_y = y;
    menu_open_ = 1;
    menu_hot = -1;
    sub_open = -1;
    dirty = 1;
}

#define MENU_W 270
#define MITEM_H 32
#define MSEP_H 9
static int menu_h(void) {
    int h = 12;
    for (int i = 0; i < menu_n; i++) h += menu_items[i] == MI_SEP ? MSEP_H : MITEM_H;
    return h;
}
static void menu_place(int *x, int *y) {
    *x = menu_x;
    *y = menu_y;
    if (*x + MENU_W > g.w - 4) *x = g.w - 4 - MENU_W;
    if (*y + menu_h() > g.h - 4) *y = g.h - 4 - menu_h();
    if (*x < 4) *x = 4;
    if (*y < 4) *y = 4;
}
static int menu_item_y(int i) {
    int x, y;
    menu_place(&x, &y);
    y += 6;
    for (int k = 0; k < i; k++) y += menu_items[k] == MI_SEP ? MSEP_H : MITEM_H;
    return y;
}
static void sub_place(int *x, int *y) {
    int mx, my;
    menu_place(&mx, &my);
    *x = mx + MENU_W - 6;
    if (*x + 220 > g.w - 4) *x = mx - 220 + 6;
    *y = menu_item_y(sub_open) - 6;
    if (*y + NAPPS * MITEM_H + 12 > g.h - 4) *y = g.h - 4 - (NAPPS * MITEM_H + 12);
}

static void draw_menu(void) {
    if (!menu_open_) return;
    int x, y;
    menu_place(&x, &y);
    int h = menu_h();
    shadow(x, y, MENU_W, h, 12, 10, 60);
    rrect(x, y, MENU_W, h, 12, GC_PANEL, 250);
    rline(x, y, MENU_W, h, 12, GC_EDGE, 110);
    for (int i = 0; i < menu_n; i++) {
        int iy = menu_item_y(i);
        int id = menu_items[i];
        if (id == MI_SEP) { rrect(x + 12, iy + 4, MENU_W - 24, 1, 0, GC_LINE, 255); continue; }
        if (i == menu_hot || i == sub_open) rrect(x + 6, iy, MENU_W - 12, MITEM_H, 8, GC_ACCENT, i == menu_hot ? 255 : 60);
        unsigned c = i == menu_hot ? 0xFFFFFF : (id == MI_DELETE ? GC_WARN : GC_TEXT);
        td(&F_UI, x + 18, iy, MITEM_H, menu_label(id), c);
        const char *k = menu_key(id);
        if (k[0]) td(&F_SMALL, x + MENU_W - 16 - tw(&F_SMALL, k), iy, MITEM_H, k, i == menu_hot ? 0xE8F0FF : GC_DIM);
        if (id == MI_OPENWITH) {
            float ax = (float)(x + MENU_W - 22), ay = iy + MITEM_H / 2.f;
            tri(ax, ay - 4, ax + 5, ay, ax, ay + 4, c, 255);
        }
    }
    if (sub_open >= 0) {
        int sx, sy;
        sub_place(&sx, &sy);
        int sh = NAPPS * MITEM_H + 12;
        shadow(sx, sy, 220, sh, 12, 10, 60);
        rrect(sx, sy, 220, sh, 12, GC_PANEL, 250);
        rline(sx, sy, 220, sh, 12, GC_EDGE, 110);
        for (int k = 0; k < NAPPS; k++) {
            int iy = sy + 6 + k * MITEM_H;
            if (k == sub_hot) rrect(sx + 6, iy, 208, MITEM_H, 8, GC_ACCENT, 255);
            td(&F_UI, sx + 18, iy, MITEM_H, en ? apps[k].en : apps[k].ru, k == sub_hot ? 0xFFFFFF : GC_TEXT);
        }
    }
}

static int menu_hit(int mx, int my, int *sub) {
    *sub = -1;
    if (!menu_open_) return -1;
    if (sub_open >= 0) {
        int sx, sy;
        sub_place(&sx, &sy);
        if (mx >= sx && mx < sx + 220 && my >= sy + 6 && my < sy + 6 + NAPPS * MITEM_H) {
            *sub = (my - sy - 6) / MITEM_H;
            return -2;
        }
    }
    int x, y;
    menu_place(&x, &y);
    if (mx < x || mx >= x + MENU_W) return -1;
    for (int i = 0; i < menu_n; i++) {
        int iy = menu_item_y(i);
        int ih = menu_items[i] == MI_SEP ? MSEP_H : MITEM_H;
        if (my >= iy && my < iy + ih) return menu_items[i] == MI_SEP ? -1 : i;
    }
    return -1;
}

/* --- properties ------------------------------------------------------------------------------ */

static int props;                    /* the card is up */
static char props_path[PATHMAX];
static int props_vol = -1;           /* it is about a drive: index into vols */
static unsigned long long props_bytes;
static int props_files, props_dirs, props_counting;
static char props_stack[64][PATHMAX];
static int props_sp;

static void props_show(const char *path, int vol) {
    snprintf(props_path, sizeof props_path, "%s", path);
    props_vol = vol;
    props = 1;
    props_bytes = 0;
    props_files = props_dirs = 0;
    props_sp = 0;
    props_counting = vol < 0 && is_dir(path);
    if (props_counting) snprintf(props_stack[props_sp++], PATHMAX, "%s", path);
    dirty = 1;
}

/* Count a folder's contents a little at a time. */
static void props_work(void) {
    if (!props || !props_counting) return;
    static struct xdirent buf[512];
    unsigned long long t0 = uptime_ms();
    while (props_sp > 0 && uptime_ms() - t0 < 20) {
        char dir[PATHMAX];
        snprintf(dir, sizeof dir, "%s", props_stack[--props_sp]);
        int n = readdir_x(dir, buf, 512);
        if (n > 512) n = 512;
        for (int i = 0; i < n; i++) {
            if (strcmp(buf[i].name, ".") == 0 || strcmp(buf[i].name, "..") == 0) continue;
            if (buf[i].is_dir) {
                props_dirs++;
                if (props_sp < 64) path_join(props_stack[props_sp++], dir, buf[i].name);
            } else {
                props_files++;
                props_bytes += buf[i].size;
            }
        }
    }
    if (!props_sp) props_counting = 0;
    dirty = 1;
}

static void props_rect(int *x, int *y, int *w, int *h) {
    *w = 440; *h = 300;
    *x = (g.w - *w) / 2;
    *y = (g.h - *h) / 2;
}

static void draw_props(void) {
    if (!props) return;
    int x, y, w, h;
    props_rect(&x, &y, &w, &h);
    rrect(0, 0, g.w, g.h, 0, 0x000000, 60);
    shadow(x, y, w, h, 16, 14, 80);
    rrect(x, y, w, h, 16, GC_PANEL, 255);
    struct stat st;
    int isd = is_dir(props_path);
    int k = props_vol >= 0 ? (vols[props_vol].index < 0 ? K_DRIVE : K_USB) : kind_of(path_base(props_path), isd, 0);
    draw_icon(k, x + 24, y + 24, 56, path_base(props_path));
    char parent[PATHMAX];
    path_parent(parent, props_path);
    const char *name = props_vol >= 0 ? vol_title(&vols[props_vol]) : disp_name(props_path, path_base(props_path));
    td_fit(&F_BIG, x + 96, y + 26, 30, name, w - 120, GC_TEXT);
    td(&F_UI, x + 96, y + 54, 22, props_vol >= 0 ? (vols[props_vol].index < 0 ? T("Системный диск", "System disk")
                                                       : T("Съёмный диск", "Removable drive"))
                                                 : kind_name(k, path_base(props_path)), GC_DIM);
    rrect(x + 24, y + 96, w - 48, 1, 0, GC_LINE, 255);
    int ly = y + 108;
    char val[300];
    #define ROWP(label, text) do { td(&F_UI, x + 24, ly, 26, label, GC_DIM); \
                                   td_fit(&F_UI, x + 150, ly, 26, text, w - 174, GC_TEXT); ly += 30; } while (0)
    if (props_vol >= 0) {
        const struct uvol *v = &vols[props_vol];
        ROWP(T("Где", "Location"), v->mount);
        ROWP(T("Файловая система", "File system"), v->fs);
        char a[32], b[32];
        fmt_size(a, sizeof a, v->total);
        ROWP(T("Объём", "Capacity"), a);
        fmt_size(b, sizeof b, v->free);
        ROWP(T("Свободно", "Free"), b);
        if (v->drive[0]) ROWP(T("Устройство", "Device"), v->drive);
    } else {
        ROWP(T("Где", "Location"), parent);
        if (isd) {
            char sz[32];
            fmt_size(sz, sizeof sz, props_bytes);
            snprintf(val, sizeof val, T("%s, файлов: %d, папок: %d%s", "%s, %d files, %d folders%s"), sz,
                     props_files, props_dirs, props_counting ? "…" : "");
            ROWP(T("Содержит", "Contains"), val);
        } else if (stat(props_path, &st) == 0) {
            char sz[32];
            fmt_size(sz, sizeof sz, (unsigned long long)st.st_size);
            snprintf(val, sizeof val, T("%s (%llu байт)", "%s (%llu bytes)"), sz, (unsigned long long)st.st_size);
            ROWP(T("Размер", "Size"), val);
        }
        if (stat(props_path, &st) == 0) {
            fmt_date(val, sizeof val, (long long)st.st_mtime);
            ROWP(T("Изменён", "Modified"), val);
        }
    }
    #undef ROWP
    button(x + w - 24 - 120, y + h - 24 - 36, 120, 36, T("Закрыть", "Close"), 1, 0, 1);
}

/* --- the confirm card and the job's progress ---------------------------------------------- */

static int conf_hot = -1;
static void conf_rect(int *x, int *y, int *w, int *h) { *w = 460; *h = 170; *x = (g.w - *w) / 2; *y = (g.h - *h) / 2; }
static void conf_btn(int which, int *x, int *y, int *w, int *h) {
    int cx, cy, cw, ch;
    conf_rect(&cx, &cy, &cw, &ch);
    *w = 140; *h = 36; *y = cy + ch - 20 - 36;
    *x = which ? cx + cw - 20 - 140 : cx + cw - 20 - 140 - 10 - 140;
}

static void draw_confirm(void) {
    if (!confirm) return;
    int x, y, w, h;
    conf_rect(&x, &y, &w, &h);
    rrect(0, 0, g.w, g.h, 0, 0x000000, 60);
    shadow(x, y, w, h, 16, 14, 80);
    rrect(x, y, w, h, 16, GC_PANEL, 255);
    const char *title = confirm == CONFIRM_DELETE ? T("Удалить?", "Delete?") : T("Заменить?", "Replace?");
    td(&F_BIG, x + 24, y + 20, 30, title, GC_TEXT);
    /* two lines at most */
    const char *s = confirm_text;
    int at = 0;
    size_t len = strlen(s), brk = have_txt ? txt_split(&F_UI, s, len, w - 48, &at) : len;
    char l1[300];
    snprintf(l1, sizeof l1, "%.*s", (int)brk, s);
    td(&F_UI, x + 24, y + 56, 24, l1, GC_DIM);
    if (brk < len) td_fit(&F_UI, x + 24, y + 80, 24, s + brk + (s[brk] == ' '), w - 48, GC_DIM);
    int bx, by, bw, bh;
    conf_btn(0, &bx, &by, &bw, &bh);
    button(bx, by, bw, bh, T("Отмена", "Cancel"), 0, conf_hot == 0, 1);
    conf_btn(1, &bx, &by, &bw, &bh);
    button(bx, by, bw, bh, confirm == CONFIRM_DELETE ? T("Удалить", "Delete") : T("Заменить", "Replace"), 1,
           conf_hot == 1, 1);
}

static void ask_delete(void) {
    int n = nsel();
    if (!n || job || strcmp(cwd, "/") == 0) return;
    if (n == 1) {
        for (int i = 0; i < nitems; i++) if (items[i].sel)
            snprintf(confirm_text, sizeof confirm_text,
                     T("«%s» будет удалён без возможности восстановить.", "\"%s\" will be deleted for good."),
                     items[i].disp);
    } else snprintf(confirm_text, sizeof confirm_text,
                    T("Объекты (%d) будут удалены без возможности восстановить.", "%d items will be deleted for good."), n);
    confirm = CONFIRM_DELETE;
    conf_hot = 1;
    dirty = 1;
}

static int job_cancel_hot;
static void job_rect(int *x, int *y, int *w, int *h) { *w = 420; *h = 118; *x = g.w - *w - 20; *y = g.h - bottom_h() - *h - 16; }

static void draw_job(void) {
    if (!job) return;
    int x, y, w, h;
    job_rect(&x, &y, &w, &h);
    shadow(x, y, w, h, 14, 12, 70);
    rrect(x, y, w, h, 14, GC_PANEL, 255);
    rline(x, y, w, h, 14, GC_EDGE, 100);
    const char *what = job == JOB_DELETE ? T("Удаление", "Deleting") : job == JOB_MOVE ? T("Перемещение", "Moving")
                                                                                        : T("Копирование", "Copying");
    char t[128];
    snprintf(t, sizeof t, "%s · %d / %d", what, at_step < nsteps ? at_step + 1 : nsteps, nsteps);
    td(&F_BOLD, x + 18, y + 12, 24, t, GC_TEXT);
    const char *cur = at_step < nsteps ? path_base(steps[at_step].src ? steps[at_step].src : steps[at_step].dst) : "";
    td_fit(&F_SMALL, x + 18, y + 38, 20, cur, w - 36, GC_DIM);
    int bx = x + 18, by = y + 66, bw = w - 36 - 110;
    rrect(bx, by, bw, 8, 4, GC_LINE, 255);
    unsigned long long tot = job_total ? job_total : 1, done = job_total ? job_done : (unsigned long long)at_step;
    if (!job_total) tot = nsteps ? (unsigned long long)nsteps : 1;
    int fw = (int)((unsigned long long)bw * (done > tot ? tot : done) / tot);
    if (fw > 0) rrect(bx, by, fw < 8 ? 8 : fw, 8, 4, GC_ACCENT, 255);
    if (job_total) {
        char a[32], b[32], line[96];
        fmt_size(a, sizeof a, job_done);
        fmt_size(b, sizeof b, job_total);
        unsigned long long el = uptime_ms() - job_t0;
        char sp[32] = "";
        if (el > 500) { fmt_size(sp, sizeof sp, job_done * 1000 / el); }
        snprintf(line, sizeof line, T("%s из %s%s%s%s", "%s of %s%s%s%s"), a, b, sp[0] ? " · " : "", sp,
                 sp[0] ? T("/с", "/s") : "");
        td(&F_SMALL, bx, by + 12, 20, line, GC_DIM);
    }
    button(x + w - 18 - 100, y + h - 18 - 34, 100, 34, T("Отмена", "Cancel"), 0, job_cancel_hot, 1);
}

/* --- drawing it all -------------------------------------------------------------------------- */

static void draw(int blink) {
    clip_all();
    tt.px = g.px;
    tt.stride = g.w;
    gui_clear(&g, GC_WIN);
    draw_side();
    draw_toolbar(blink);
    draw_files(blink);
    draw_bottom(blink);
    draw_job();
    draw_menu();
    draw_props();
    draw_confirm();
}

/* --- input -------------------------------------------------------------------------------------- */

static unsigned dbl_ms = 400;
static unsigned long long last_click_t;
static int last_click_vi = -1;
static int press_vi = -1, press_x, press_y, may_drag, dragging_out;
static int sb_drag, sb_grab;

static void select_only(int vi) {
    clear_sel();
    if (vi >= 0 && vi < nview) items[view[vi]].sel = 1;
    cursor = anchor = vi;
}

static void select_range(int a, int b) {
    clear_sel();
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = a; i <= b; i++) if (i >= 0 && i < nview) items[view[i]].sel = 1;
}

static void key_move(int to, int shift) {
    if (!nview) return;
    if (to < 0) to = 0;
    if (to >= nview) to = nview - 1;
    if (shift && anchor >= 0) { select_range(anchor, to); cursor = to; }
    else select_only(to);
    if (pick == PICK_SAVE && !items[view[to]].dir) ed_set(&pick_name, items[view[to]].name, 0);
    scroll_to(to);
}

/* Type-ahead: letters typed quickly go to the first name that starts so. */
static char ahead[64];
static unsigned long long ahead_t;
static void type_ahead(char c) {
    unsigned long long now = uptime_ms();
    if (now - ahead_t > 900) ahead[0] = 0;
    ahead_t = now;
    size_t l = strlen(ahead);
    if (l < sizeof ahead - 1) { ahead[l] = c; ahead[l + 1] = 0; }
    /* only once a whole character is in */
    if (((unsigned char)c & 0xC0) == 0xC0) return;
    for (int i = 0; i < nview; i++) {
        const char *a = items[view[i]].disp, *b = ahead;
        int ok = 1;
        for (;;) {
            if (!*b) break;
            unsigned x = fold(next_cp(&a)), y = fold(next_cp(&b));
            if (x != y) { ok = 0; break; }
        }
        if (ok) { key_move(i, 0); return; }
    }
}

static void menu_do(int id, int app) {
    menu_open_ = 0;
    dirty = 1;
    if (menu_side >= 0) {
        side_row *r = &srows[menu_side];
        menu_side = -1;
        if (id == MI_OPEN) go(r->kind ? vols[r->idx].mount : stdf[r->idx].path, 1);
        if (id == MI_EJECT && r->kind == 1) {
            const struct uvol *v = &vols[r->idx];
            char t[96];
            snprintf(t, sizeof t, "%s", vol_title(v));
            if (strncmp(cwd, v->mount, strlen(v->mount)) == 0) go("/home", 1);
            if (volume_eject(v->index) == 0) {
                notify(T("Можно извлечь", "Safe to remove"), t);
                set_status(T("Флешку можно вынимать", "The drive can be removed"));
            }
            refresh_volumes();
        }
        if (id == MI_PROPS) props_show(r->kind ? vols[r->idx].mount : stdf[r->idx].path, r->kind ? r->idx : -1);
        return;
    }
    switch (id) {
    case MI_OPEN: {
        int vi = cursor;
        for (int i = 0; i < nview; i++) if (items[view[i]].sel) { vi = i; break; }
        open_item(vi, 0);
        break;
    }
    case MI_CUT: do_copy(1); break;
    case MI_COPY: do_copy(0); break;
    case MI_PASTE: do_paste(); break;
    case MI_RENAME:
        for (int i = 0; i < nview; i++) if (items[view[i]].sel) { begin_rename(i); break; }
        break;
    case MI_DELETE: ask_delete(); break;
    case MI_NEWDIR: new_folder(); break;
    case MI_NEWFILE: new_text_file(); break;
    case MI_VIEW: grid = !grid; scroll_y = 0; break;
    case MI_HIDDEN: show_hidden = !show_hidden; build_view(); break;
    case MI_REFRESH: load(); refresh_volumes(); break;
    case MI_PROPS: {
        int vi = -1;
        for (int i = 0; i < nview; i++) if (items[view[i]].sel) { vi = i; break; }
        if (vi >= 0) {
            char full[PATHMAX];
            path_join(full, cwd, items[view[vi]].name);
            props_show(full, -1);
        } else props_show(cwd, -1);
        break;
    }
    default:
        if (id == MI_OPENWITH && app >= 0) {
            for (int i = 0; i < nview; i++) if (items[view[i]].sel) { open_item(i, apps[app].path); break; }
        }
    }
}

static void on_key(const key_event_t *k) {
    dirty = 1;
    int ctrl = k->mods & XMOD_CTRL, shift = k->mods & XMOD_SHIFT, alt = k->mods & XMOD_ALT;
    if (confirm) {
        if (k->code == XKEY_ESC) confirm = 0;
        if (k->code == XKEY_ENTER) {
            if (confirm == CONFIRM_DELETE) do_delete();
            else if (confirm == CONFIRM_OVERWRITE) pick_finish(pending_save);
            confirm = 0;
        }
        return;
    }
    if (props) { if (k->code == XKEY_ESC || k->code == XKEY_ENTER) props = 0; return; }
    if (menu_open_) { if (k->code == XKEY_ESC) { menu_open_ = 0; menu_side = -1; } return; }
    if (ren_item >= 0) {
        if (k->code == XKEY_ENTER) end_rename(1);
        else if (k->code == XKEY_ESC) end_rename(0);
        else ed_key(&ren, k);
        return;
    }
    if (focus_field == 1) {
        if (k->code == XKEY_ESC) { srch.s[0] = 0; srch.cur = 0; focus_field = 0; }
        else if (k->code == XKEY_ENTER || k->code == XKEY_DOWN) { focus_field = 0; if (nview) key_move(0, 0); }
        else ed_key(&srch, k);
        snprintf(search, sizeof search, "%s", srch.s);
        build_view();
        scroll_y = 0;
        return;
    }
    if (focus_field == 2) {
        if (k->code == XKEY_ESC) focus_field = 0;
        else if (k->code == XKEY_ENTER) { focus_field = 0; go(pathed.s, 1); }
        else ed_key(&pathed, k);
        return;
    }
    if (focus_field == 3) {
        if (k->code == XKEY_ENTER) { pick_accept(); return; }
        if (k->code == XKEY_ESC) { pick_finish(0); return; }
        if (k->code == XKEY_CHAR && k->ascii == '\t') { focus_field = 0; return; }
        if (k->code == XKEY_UP || k->code == XKEY_DOWN) focus_field = 0;
        else { ed_key(&pick_name, k); return; }
    }

    if (ctrl && k->code == XKEY_CHAR) {
        switch (k->ascii | 0x20) {
        case 'a': for (int i = 0; i < nview; i++) items[view[i]].sel = 1; return;
        case 'c': if (!pick) do_copy(0); return;
        case 'x': if (!pick && strcmp(cwd, "/")) do_copy(1); return;
        case 'v': if (!pick && strcmp(cwd, "/")) do_paste(); return;
        case 'f': focus_field = 1; ed_set(&srch, search, 0); return;
        case 'l': focus_field = 2; ed_set(&pathed, cwd, 1); return;
        case 'h': show_hidden = !show_hidden; build_view(); return;
        case 'n': if (shift && pick != PICK_OPEN) new_folder(); return;
        case 'w': if (!pick) running = 0; return;
        }
        return;
    }
    switch (k->code) {
    case XKEY_UP:    if (alt) go_up(); else if (grid) key_move(cursor - grid_cols(), shift); else key_move(cursor < 0 ? 0 : cursor - 1, shift); return;
    case XKEY_DOWN:  if (grid) key_move(cursor < 0 ? 0 : cursor + grid_cols(), shift); else key_move(cursor + 1, shift); return;
    case XKEY_LEFT:  if (alt) go_back(); else if (grid) key_move(cursor - 1, shift); return;
    case XKEY_RIGHT: if (alt) go_fwd(); else if (grid) key_move(cursor + 1, shift); return;
    case XKEY_HOME:  key_move(0, shift); return;
    case XKEY_END:   key_move(nview - 1, shift); return;
    case XKEY_PGUP:  key_move(cursor - content_h() / (grid ? TILE_H : ROW_H) * (grid ? grid_cols() : 1), shift); return;
    case XKEY_PGDN:  key_move(cursor + content_h() / (grid ? TILE_H : ROW_H) * (grid ? grid_cols() : 1), shift); return;
    case XKEY_ENTER:
        if (pick == PICK_SAVE && (cursor < 0 || !items[view[cursor]].dir)) { pick_accept(); return; }
        if (cursor >= 0) open_item(cursor, 0);
        return;
    case XKEY_BKSP:  go_up(); return;
    case XKEY_DEL:   if (!pick) ask_delete(); return;
    case XKEY_ESC:
        if (pick) { pick_finish(0); return; }
        if (search[0]) { search[0] = 0; srch.s[0] = 0; build_view(); return; }
        clear_sel();
        return;
    }
    if (k->code == XKEY_F(2) && !pick) { if (cursor >= 0) begin_rename(cursor); return; }
    if (k->code == XKEY_F(5)) { load(); refresh_volumes(); return; }
    if (k->code == XKEY_CHAR && k->ascii == '\t' && pick == PICK_SAVE) { focus_field = 3; return; }
    if (k->code == XKEY_CHAR && (unsigned char)k->ascii > ' ') type_ahead(k->ascii);
}

static void on_mouse(const mouse_event_t *m) {
    int mx = m->x, my = m->y;
    int was_hot = hot, was_side = side_hot, was_ej = side_eject_hot, was_tb = tb_hot, was_cr = crumb_hot,
        was_pb = pb_hot, was_mh = menu_hot, was_sh = sub_hot, was_ch = conf_hot, was_jc = job_cancel_hot;

    /* something dragged over the window, or dropped on it */
    if (m->drag) {
        int was_d = drop_vi, was_h = drop_here;
        drop_vi = -1;
        drop_here = 0;
        if (m->drag != DRAG_LEAVE) {
            int vi = item_at(mx, my);
            int si = side_at(mx, my);
            if (vi >= 0 && items[view[vi]].dir && !items[view[vi]].sel) drop_vi = vi;
            else if (si < 0 && mx >= SIDE_W && strcmp(cwd, "/") != 0) drop_here = 1;
            if (si >= 0) side_hot = si;
            if (m->drag == DRAG_DROP) {
                static char payload[65536];
                int n = drag_take(payload, sizeof payload - 1);
                if (n > 0) {
                    payload[n] = 0;
                    char dst[PATHMAX] = "";
                    if (drop_vi >= 0) path_join(dst, cwd, items[view[drop_vi]].name);
                    else if (si >= 0) snprintf(dst, sizeof dst, "%s", srows[si].kind ? vols[srows[si].idx].mount
                                                                                      : stdf[srows[si].idx].path);
                    else if (drop_here) snprintf(dst, sizeof dst, "%s", cwd);
                    /* Moved on the same drive, copied to another -- as on Windows. */
                    if (dst[0] && strcmp(dst, "/") != 0) {
                        char first[PATHMAX];
                        int fl = 0;
                        while (payload[fl] && payload[fl] != '\n' && fl < PATHMAX - 1) { first[fl] = payload[fl]; fl++; }
                        first[fl] = 0;
                        bring(payload, dst, same_drive(first, dst));
                    }
                }
                drop_vi = -1;
                drop_here = 0;
                side_hot = -1;
                dragging_out = 0;
                may_drag = 0;
            }
        }
        if (drop_vi != was_d || drop_here != was_h || side_hot != was_side) dirty = 1;
        return;
    }

    /* dialogs own the pointer while they are up */
    if (confirm) {
        conf_hot = -1;
        for (int b = 0; b < 2; b++) {
            int x, y, w, h;
            conf_btn(b, &x, &y, &w, &h);
            if (gui_in(mx, my, x, y, w, h)) conf_hot = b;
        }
        if (m->pressed & MB_LEFT) {
            if (conf_hot == 1) {
                if (confirm == CONFIRM_DELETE) do_delete();
                else if (confirm == CONFIRM_OVERWRITE) pick_finish(pending_save);
                confirm = 0;
            } else if (conf_hot == 0) confirm = 0;
            dirty = 1;
        }
        if (conf_hot != was_ch) dirty = 1;
        return;
    }
    if (props) {
        if (m->pressed & MB_LEFT) {
            int x, y, w, h;
            props_rect(&x, &y, &w, &h);
            if (!gui_in(mx, my, x, y, w, h) || gui_in(mx, my, x + w - 144, y + h - 60, 120, 36)) props = 0;
            dirty = 1;
        }
        return;
    }
    if (menu_open_) {
        int sub;
        int i = menu_hit(mx, my, &sub);
        if (i >= 0) {
            menu_hot = i;
            sub_hot = -1;
            if (menu_items[i] == MI_OPENWITH) sub_open = i;
            else sub_open = -1;
        } else if (i == -2) { sub_hot = sub; }
        else { menu_hot = -1; sub_hot = -1; }
        if (m->pressed & (MB_LEFT | MB_RIGHT)) {
            if (i >= 0 && menu_items[i] != MI_OPENWITH) menu_do(menu_items[i], -1);
            else if (i == -2 && sub >= 0) menu_do(MI_OPENWITH, sub);
            else if (i < 0) { menu_open_ = 0; menu_side = -1; dirty = 1; }
        }
        if (menu_hot != was_mh || sub_hot != was_sh) dirty = 1;
        return;
    }
    if (job) {
        int x, y, w, h;
        job_rect(&x, &y, &w, &h);
        job_cancel_hot = gui_in(mx, my, x + w - 118, y + h - 52, 100, 34);
        if ((m->pressed & MB_LEFT) && job_cancel_hot) { job_cancel = 1; return; }
        if (job_cancel_hot != was_jc) dirty = 1;
    }

    /* the scroll bar */
    if (sb_drag) {
        if (!(m->buttons & MB_LEFT)) sb_drag = 0;
        else {
            int th = total_h(), vh = content_h();
            int bh = vh * vh / th;
            if (bh < 30) bh = 30;
            int pos = my - content_y() - sb_grab;
            scroll_y = vh - bh > 0 ? pos * (th - vh) / (vh - bh) : 0;
            clamp_scroll();
            dirty = 1;
        }
        return;
    }
    /* the rubber band */
    if (band) {
        if (!(m->buttons & MB_LEFT)) { band = 0; dirty = 1; return; }
        band_x1 = mx; band_y1 = my;
        int bx0 = band_x0 < band_x1 ? band_x0 : band_x1, bx1 = band_x0 < band_x1 ? band_x1 : band_x0;
        int by0 = band_y0 < band_y1 ? band_y0 : band_y1, by1 = band_y0 < band_y1 ? band_y1 : band_y0;
        clear_sel();
        for (int i = 0; i < nview; i++) {
            int x, y, w, h;
            item_rect(i, &x, &y, &w, &h);
            if (!grid) { w = 300; }
            if (x < bx1 && x + w > bx0 && y < by1 && y + h > by0) { items[view[i]].sel = 1; cursor = i; }
        }
        if (my < content_y() + 10) { scroll_y -= 12; clamp_scroll(); }
        if (my > content_y() + content_h() - 10) { scroll_y += 12; clamp_scroll(); }
        dirty = 1;
        return;
    }
    /* a press that moved: a drag, handed to the window manager */
    if (may_drag && (m->buttons & MB_LEFT) && press_vi >= 0 && !dragging_out) {
        if (abs(mx - press_x) + abs(my - press_y) > 8) {
            static char payload[65536];
            sel_paths(payload, sizeof payload, 0);
            size_t l = strlen(payload);
            if (l && payload[l - 1] == '\n') payload[l - 1] = 0;
            char label[300];
            int n = nsel();
            if (n == 1) snprintf(label, sizeof label, "%s", items[view[press_vi]].disp);
            else snprintf(label, sizeof label, T("Объектов: %d", "%d items"), n);
            if (l) { drag_begin(payload, label); dragging_out = 1; }
            may_drag = 0;
        }
        return;
    }
    if (m->released & MB_LEFT) { may_drag = 0; dragging_out = 0; }

    /* hover */
    hot = item_at(mx, my);
    side_hot = side_at(mx, my);
    side_eject_hot = -1;
    if (side_hot >= 0 && srows[side_hot].kind == 1 && vols[srows[side_hot].idx].index >= 0) {
        int x, y, w, h;
        eject_rect(&srows[side_hot], &x, &y, &w, &h);
        if (gui_in(mx, my, x, y, w, h)) side_eject_hot = side_hot;
    }
    tb_hot = -1;
    for (int b = TB_BACK; b <= TB_NEW; b++) {
        int x, y, w, h;
        tb_rect(b, &x, &y, &w, &h);
        if (gui_in(mx, my, x, y, w, h)) tb_hot = b;
    }
    crumb_hot = -1;
    {
        int cx, cy, cw, ch;
        crumbs_rect(&cx, &cy, &cw, &ch);
        if (focus_field != 2 && gui_in(mx, my, cx, cy, cw, ch))
            for (int i = 0; i < ncrumbs; i++) if (crumbs[i].x >= 0 && mx >= crumbs[i].x && mx < crumbs[i].x + crumbs[i].w) crumb_hot = i;
    }
    pb_hot = -1;
    if (pick) for (int b = 0; b < 2; b++) {
        int x, y, w, h;
        pb_rect(b, &x, &y, &w, &h);
        if (gui_in(mx, my, x, y, w, h)) pb_hot = b;
    }
    if (hot != was_hot || side_hot != was_side || side_eject_hot != was_ej || tb_hot != was_tb ||
        crumb_hot != was_cr || pb_hot != was_pb) dirty = 1;

    if (m->wheel) {
        if (mx >= SIDE_W) { scroll_y -= m->wheel * (grid ? TILE_H / 2 : ROW_H * 3); clamp_scroll(); dirty = 1; }
        return;
    }

    if (m->pressed & MB_RIGHT) {
        if (ren_item >= 0) end_rename(1);
        int si = side_at(mx, my);
        if (si >= 0) { menu_side = si; menu_show(mx, my, 0); return; }
        menu_side = -1;
        int vi = item_at(mx, my);
        if (vi >= 0) {
            if (!items[view[vi]].sel) select_only(vi);
            menu_show(mx, my, 1);
        } else if (mx >= SIDE_W && my >= content_y()) {
            clear_sel();
            menu_show(mx, my, 0);
        }
        return;
    }
    if (!(m->pressed & MB_LEFT)) return;
    dirty = 1;
    if (ren_item >= 0) {
        int vi = item_at(mx, my);
        if (vi < 0 || view[vi] != ren_item) end_rename(1);
        else return;
    }

    /* the bar along the bottom of a picker */
    if (pick) {
        if (pb_hot == PB_CANCEL) { pick_finish(0); return; }
        if (pb_hot == PB_OK) { pick_accept(); return; }
        if (pick == PICK_SAVE) {
            int x, y, w, h;
            pname_rect(&x, &y, &w, &h);
            if (gui_in(mx, my, x, y, w, h)) { focus_field = 3; pick_name.all = 0; return; }
        }
    }
    /* the side bar */
    if (side_hot >= 0) {
        side_row *r = &srows[side_hot];
        if (side_eject_hot >= 0) { menu_side = side_hot; menu_do(MI_EJECT, -1); return; }
        go(r->kind ? vols[r->idx].mount : stdf[r->idx].path, 1);
        return;
    }
    /* the tool bar */
    if (tb_hot >= 0) {
        switch (tb_hot) {
        case TB_BACK: go_back(); break;
        case TB_FWD: go_fwd(); break;
        case TB_UP: go_up(); break;
        case TB_GRID: grid = !grid; scroll_y = 0; break;
        case TB_NEW: if (pick != PICK_OPEN) new_folder(); break;
        }
        return;
    }
    {
        int sx, sy, sw, sh;
        search_rect(&sx, &sy, &sw, &sh);
        if (gui_in(mx, my, sx, sy, sw, sh)) { focus_field = 1; ed_set(&srch, search, 0); return; }
        int cx, cy, cw, ch;
        crumbs_rect(&cx, &cy, &cw, &ch);
        if (gui_in(mx, my, cx, cy, cw, ch)) {
            if (crumb_hot >= 0 && crumb_hot < ncrumbs - 1) go(crumbs[crumb_hot].path, 1);
            else if (focus_field != 2) { focus_field = 2; ed_set(&pathed, cwd, 1); }
            return;
        }
    }
    if (focus_field == 1 || focus_field == 2) focus_field = 0;
    if (focus_field == 3 && my < g.h - PICK_H) focus_field = 0;
    if (my < content_y()) {
        /* the column headings sort */
        if (!grid && my >= TOP_H && mx >= SIDE_W) {
            int xn, xd, xs, xt, xe;
            cols(&xn, &xd, &xs, &xt, &xe);
            int c = mx < xd ? 0 : mx < xs ? 1 : mx < xt ? 2 : 3;
            if (sort_col == c) sort_desc = !sort_desc;
            else { sort_col = c; sort_desc = 0; }
            build_view();
        }
        return;
    }
    if (mx >= g.w - 12 && total_h() > content_h()) {
        int th = total_h(), vh = content_h();
        int bh = vh * vh / th;
        if (bh < 30) bh = 30;
        int by = content_y() + (vh - bh) * scroll_y / (th - vh);
        sb_drag = 1;
        sb_grab = my >= by && my < by + bh ? my - by : bh / 2;
        return;
    }
    int vi = item_at(mx, my);
    if (vi < 0) {
        if (mx >= SIDE_W && my >= content_y() && my < content_y() + content_h()) {
            clear_sel();
            cursor = -1;
            band = 1;
            band_x0 = band_x1 = mx;
            band_y0 = band_y1 = my;
        }
        return;
    }
    unsigned long long now = uptime_ms();
    if (vi == last_click_vi && now - last_click_t < dbl_ms) {
        last_click_vi = -1;
        open_item(vi, 0);
        return;
    }
    last_click_vi = vi;
    last_click_t = now;
    /* With Ctrl -- held keys come with the key events, not the pointer's, so
     * the last key's modifiers stand for them. */
    if (last_mods & XMOD_CTRL) { items[view[vi]].sel = !items[view[vi]].sel; cursor = anchor = vi; }
    else if ((last_mods & XMOD_SHIFT) && anchor >= 0) { select_range(anchor, vi); cursor = vi; }
    else if (!items[view[vi]].sel) select_only(vi);
    else cursor = anchor = vi;
    if (pick == PICK_SAVE && !items[view[vi]].dir) ed_set(&pick_name, items[view[vi]].name, 0);
    press_vi = vi;
    press_x = mx;
    press_y = my;
    may_drag = !pick;
}

/* --- the picker's request ------------------------------------------------------------------- */

/* --pick=FILE: FILE has one item a line -- the mode (open, save, folder), the
 * title, the folder to start in, the name to suggest, the extensions to show
 * ("txt,c,h"), and what to call them. The answer goes to FILE.out: the path
 * chosen, or nothing at all for Cancel. */
static void read_request(const char *file) {
    snprintf(pick_req, sizeof pick_req, "%s", file);
    char buf[2048];
    int fd = open(file, O_RDONLY);
    int n = fd >= 0 ? (int)read(fd, buf, sizeof buf - 1) : 0;
    if (fd >= 0) close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    char *line[6] = { 0 };
    char *p = buf;
    for (int i = 0; i < 6 && *p; i++) {
        line[i] = p;
        char *e = strchr(p, '\n');
        if (!e) break;
        *e = 0;
        p = e + 1;
    }
    pick = PICK_OPEN;
    if (line[0] && strcmp(line[0], "save") == 0) pick = PICK_SAVE;
    if (line[0] && strcmp(line[0], "folder") == 0) pick = PICK_DIR;
    if (line[1]) snprintf(pick_title, sizeof pick_title, "%s", line[1]);
    if (line[2] && line[2][0] && is_dir(line[2])) snprintf(cwd, sizeof cwd, "%s", line[2]);
    else snprintf(cwd, sizeof cwd, "%s", is_dir("/home/Documents") ? "/home/Documents" : "/home");
    if (line[3]) ed_set_name(&pick_name, line[3]);
    if (line[4]) snprintf(pick_exts, sizeof pick_exts, "%s", line[4]);
    if (line[5]) snprintf(pick_label, sizeof pick_label, "%s", line[5]);
    if (pick == PICK_SAVE) focus_field = 3;
}

int main(int argc, char **argv) {
    en = ui_lang() == 1;
    have_txt = txt_init(0) > 0;
    /* the standard folders, made the first time */
    if (is_dir("/home")) for (int i = 1; i < NSTD; i++) if (!is_dir(stdf[i].path)) mkdir(stdf[i].path, 0755);
    if (argc > 1 && strncmp(argv[1], "--pick=", 7) == 0) read_request(argv[1] + 7);
    else if (argc > 1 && is_dir(argv[1])) snprintf(cwd, sizeof cwd, "%s", argv[1]);
    if (!gui_open(&g)) return 1;
    dbl_ms = (unsigned)xyuos_syscall3(SYS_SETTING, SETOP_GET, SET_DBLCLICK, 0);
    if (dbl_ms < 100 || dbl_ms > 2000) dbl_ms = 400;
    refresh_volumes();
    char start[PATHMAX];
    snprintf(start, sizeof start, "%s", cwd);
    cwd[0] = 0;
    go(start, 1);
    if (!nitems && !is_dir(start)) go("/", 1);

    unsigned long long last_vol = uptime_ms(), last_blink_t = 0;
    int blink = 1;
    while (running) {
        gui_event_t e;
        int any = 0;
        while (gui_poll(&g, &e)) {
            any = 1;
            if (e.type == GE_KEY) {
                if (e.k.code == XKEY_RESIZE) { gui_sync(&g); dirty = 1; continue; }
                if (e.k.code == XKEY_FOCUSOUT) continue;
                last_mods = e.k.mods;
                if (!e.k.pressed) continue;
                blink = 1;
                last_blink_t = uptime_ms();
                on_key(&e.k);
            } else if (e.type == GE_MOUSE) {
                on_mouse(&e.m);
            }
            if (!running) break;
        }
        if (!running) break;
        job_work();
        props_work();
        unsigned long long now = uptime_ms();
        if (now - last_vol > 1500) {
            last_vol = now;
            unsigned st = vols_stamp;
            refresh_volumes();
            if (st != vols_stamp) {
                /* the drive on show went away: home */
                if (strncmp(cwd, "/usb", 4) == 0 && !vol_for(cwd)) go("/home", 1);
                else if (strcmp(cwd, "/") == 0) load();
                dirty = 1;
            }
        }
        if ((ren_item >= 0 || focus_field) && now - last_blink_t > 530) {
            last_blink_t = now;
            blink = !blink;
            dirty = 1;
        }
        if (dirty) {
            if (gui_sync(&g)) {
                clamp_scroll();
                draw(blink);
                gui_present(&g);
                dirty = 0;
            } else if (gui_lost(&g)) break;
        }
        if (!any && !job && !props_counting) sleep_ms(12);
    }
    if (pick && running == 0) {
        /* closed with the window's own button: the answer is "cancel" -- the
         * caller notices the process went without one. */
    }
    gui_close(&g);
    return 0;
}
