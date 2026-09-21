/* files -- the graphical file manager.
 *
 * The older `fm` draws a character grid and owns the whole pane; this one is a
 * window with a pointer in it. That difference is not cosmetic: a text-grid
 * browser can only ever show what fits in cells, so a file's icon, its size
 * and its name have to compete for the same columns. With a pixel surface the
 * layout is free, and the interesting work moves to where it belongs -- what
 * happens when you double-click a file.
 *
 * Opening a file does not mean "read it here". It means: look at the
 * extension, and spawn the program that owns that kind of file. That is the
 * whole of the system's file-association story, and it lives in open_file()
 * below, in about fifteen lines. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "gui.h"
#include "icons.h"
#include "upath.h"

#define MAX_ENT   512
#define NAMELEN   96

struct ent {
    char         name[NAMELEN];
    int          is_dir;
    unsigned int size;
};

static struct ent  ents[MAX_ENT];
static int         count;
static char        cwd[UPATH_MAX] = "/";
static char        iobuf[16384];

static int  sel = -1, top;
static int  hover = -1;
static char status[160];

/* Modal state. The manager is a single loop, so a dialog is a mode it is in,
 * not a nested event loop -- which is what keeps repainting correct. */
#define M_NONE   0
#define M_RENAME  1
#define M_MKDIR   2
#define M_DELETE  3
#define M_NEWFILE 4
static int        mode;
static char       edit_buf[NAMELEN];
static gui_edit_t editor = { edit_buf, NAMELEN, 0, 0 };

/* Right-click menu. It is a mode like the dialogs are: while it is up it owns
 * the pointer, so a click either picks an item or dismisses it and does
 * nothing else. Anything looser and the click that closes a menu also lands
 * on whatever was underneath, which is never what anyone means. */
/* Drag and drop.
 *
 * A press does not start a drag -- movement does. Otherwise every click on a
 * row would begin one, and selecting a file with a slightly unsteady hand
 * would move it somewhere. */
static int may_drag, press_x, press_y, drag_row = -1;
static int drop_row = -1, drop_place = -1, drop_bg;

static int menu_open, menu_x, menu_y, menu_hot = -1;
static int sub_open, sub_hot = -1;

static const char *menu_item[] = { "Open", "Rename", "Delete",
                                   "New file", "New folder", "Refresh" };
#define NMENU ((int)(sizeof menu_item / sizeof menu_item[0]))
#define MENU_NEEDS_FILE(i) ((i) < 3)
#define MENU_NEWFILE 3

/* What "New file" can make.
 *
 * The template is the point. An empty .c is a file you now have to remember
 * the shape of; one that already compiles is a starting place -- and this
 * machine has a C compiler on it, so that is a real difference rather than a
 * decoration. The header guard is filled in from the name at creation time,
 * because a guard that says H in every file is worse than none. */
static const struct { const char *label, *ext, *tmpl; } kinds[] = {
    { "Text document", ".txt", "" },
    { "C source",      ".c",
      "#include <stdio.h>\n"
      "\n"
      "int main(void) {\n"
      "    printf(\"hello\\n\");\n"
      "    return 0;\n"
      "}\n" },
    { "C header",      ".h",   0 },        /* built from the name below */
    { "Python script", ".py",
      "def main():\n"
      "    print(\"hello\")\n"
      "\n"
      "\n"
      "if __name__ == \"__main__\":\n"
      "    main()\n" },
    { "Shell script",  ".sh",  "#!/bin/sh\n" },
    { "Empty file",    "",     "" },
};
#define NKINDS ((int)(sizeof kinds / sizeof kinds[0]))

static int new_kind;

/* --- listing -------------------------------------------------------------- */

static void add_entry(const char *name, int is_dir, void *ctx) {
    (void)ctx;
    if (count >= MAX_ENT) return;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return;
    snprintf(ents[count].name, NAMELEN, "%s", name);
    ents[count].is_dir = is_dir;
    ents[count].size = 0;
    count++;
}

static void sort_entries(void) {
    /* Directories first, then case-insensitive by name. Insertion sort: the
     * lists are small and this keeps equal entries in listing order. */
    for (int i = 1; i < count; i++) {
        struct ent key = ents[i];
        int j = i - 1;
        while (j >= 0) {
            struct ent *a = &ents[j];
            int worse;
            if (a->is_dir != key.is_dir) worse = (a->is_dir < key.is_dir);
            else {
                int c = 0;
                for (int k = 0;; k++) {
                    char x = a->name[k], y = key.name[k];
                    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
                    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
                    if (x != y) { c = (unsigned char)x - (unsigned char)y; break; }
                    if (!x) break;
                }
                worse = (c > 0);
            }
            if (!worse) break;
            ents[j + 1] = ents[j];
            j--;
        }
        ents[j + 1] = key;
    }
}

static void load(void) {
    count = 0;
    long n = xyuos_listdir(cwd, iobuf, sizeof(iobuf));
    if (n > 0) upath_each_entry(iobuf, n, add_entry, 0);
    sort_entries();

    /* Sizes come from stat(), one call per entry. Cheap here because a
     * directory listing is bounded; a recursive tool would need a batch call. */
    unsigned long total = 0;
    int files = 0, dirs = 0;
    for (int i = 0; i < count; i++) {
        char full[UPATH_MAX];
        upath_resolve(cwd, ents[i].name, full);
        struct xyuos_stat st;
        if (xyuos_stat(full, &st) == 0) ents[i].size = st.size;
        if (ents[i].is_dir) dirs++;
        else { files++; total += ents[i].size; }
    }
    snprintf(status, sizeof status, "%d folders, %d files, %lu bytes",
             dirs, files, total);
    sel = count ? 0 : -1;
    top = 0;
}

static void go(const char *where) {
    char nxt[UPATH_MAX];
    upath_resolve(cwd, where, nxt);
    snprintf(cwd, sizeof cwd, "%s", nxt);
    load();
}

/* --- file associations ---------------------------------------------------- */

static int ext_is(const char *name, const char *ext) {
    int n = (int)strlen(name), e = (int)strlen(ext);
    if (n <= e + 1 || name[n - e - 1] != '.') return 0;
    for (int i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Which program owns this file? Everything the system can display goes
 * through `view`; text goes to the editor; a script goes to the shell. */
static const char *app_for(const char *name) {
    if (ext_is(name, "png") || ext_is(name, "jpg") ||
        ext_is(name, "jpeg") || ext_is(name, "bmp")) return "/bin/view";
    if (ext_is(name, "wav")) return "/bin/play";
    if (ext_is(name, "sh") || ext_is(name, "bat")) return "/bin/sh";
    /* Source, so it opens in the editor rather than running: a script that
     * ran on every double-click would be a surprise while it is still being
     * written, and `python name.py` from the terminal is the other half. */
    if (ext_is(name, "py")) return "/bin/note";
    if (ext_is(name, "elf")) return 0;
    return "/bin/note";
}

static void open_file(const char *name) {
    char full[UPATH_MAX];
    upath_resolve(cwd, name, full);

    /* Each of these gets a window of its own. Opening a document from a file
     * manager and having it appear where the file manager was is the single
     * most irritating thing a file manager can do. */
    const char *app = app_for(name);
    if (!app) {                                  /* a program: run it as-is */
        if (spawn_window(full, 0) < 0)
            snprintf(status, sizeof status, "cannot run %s", name);
        return;
    }
    if (spawn_window(app, full) < 0)
        snprintf(status, sizeof status, "cannot open %s (no %s)", name, app);
    else
        snprintf(status, sizeof status, "opened %s", name);
}

static void activate(int i) {
    if (i < 0 || i >= count) return;
    if (ents[i].is_dir) go(ents[i].name);
    else open_file(ents[i].name);
}

/* --- moving files --------------------------------------------------------- */

static char copybuf[16384];

/* Copy bytes from one path to another. Needed because rename() cannot cross a
 * filesystem boundary: dragging from the ext2 root onto the USB stick is two
 * different filesystems, and the kernel rightly refuses to pretend otherwise. */
static int copy_file(const char *src, const char *dst) {
    long in = xyuos_open(src);
    if (in < 0) return -1;
    if (xyuos_create(dst) != 0) { xyuos_close(in); return -1; }
    int out = open(dst, O_WRONLY);
    if (out < 0) { xyuos_close(in); return -1; }

    long n;
    int bad = 0;
    while ((n = xyuos_read(in, copybuf, sizeof copybuf)) > 0) {
        if (write(out, copybuf, (unsigned long)n) != n) { bad = 1; break; }
    }
    close(out);
    xyuos_close(in);
    return bad ? -1 : 0;
}

static const char *base_of(const char *path) {
    const char *b = path;
    for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

static void move_into(const char *src, const char *dstdir) {
    char dst[UPATH_MAX];
    upath_resolve(dstdir, base_of(src), dst);
    if (strcmp(src, dst) == 0) return;          /* dropped where it already is */

    if (xyuos_rename(src, dst) == 0) {
        snprintf(status, sizeof status, "moved %s to %s", base_of(src), dstdir);
    } else if (copy_file(src, dst) == 0) {
        xyuos_unlink(src);
        snprintf(status, sizeof status, "moved %s to %s", base_of(src), dstdir);
    } else {
        snprintf(status, sizeof status, "cannot move %s to %s", base_of(src), dstdir);
    }
    load();
}

/* --- layout ---------------------------------------------------------------
 * Computed fresh every frame from the current surface size, which is what
 * makes resizing free. */

#define SIDE_W   150
#define TOOL_H   40
#define PATH_H   32
#define STAT_H   26
#define SBW      14

static int row_h(gui_t *g) { return g->fh + 12; }

static void list_rect(gui_t *g, int *x, int *y, int *w, int *h) {
    *x = SIDE_W;
    *y = TOOL_H + PATH_H;
    *w = g->w - SIDE_W;
    *h = g->h - TOOL_H - PATH_H - STAT_H;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

static int visible_rows(gui_t *g) {
    int x, y, w, h;
    list_rect(g, &x, &y, &w, &h);
    int r = h / row_h(g);
    return r < 1 ? 1 : r;
}

static void scroll_to_sel(gui_t *g) {
    int vis = visible_rows(g);
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    if (top > count - vis) top = count - vis;
    if (top < 0) top = 0;
}

/* --- drawing -------------------------------------------------------------- */

static iconset_t icons;

/* Which drawing belongs to a file. The names are the ones in assets/icons/;
 * anything without a match falls back to the shapes drawn below, so a missing
 * icon costs nothing. */
static const char *icon_for(const char *name, int is_dir) {
    if (is_dir) return "folder";
    if (ext_is(name, "png"))  return "image-png";
    if (ext_is(name, "jpg") || ext_is(name, "jpeg")) return "image-jpg";
    if (ext_is(name, "bmp"))  return "image-bmp";
    if (ext_is(name, "wav"))  return "audio";
    if (ext_is(name, "elf"))  return "program-elf";
    if (ext_is(name, "sh"))   return "script-sh";
    if (ext_is(name, "bat"))  return "script-bat";
    if (ext_is(name, "c") || ext_is(name, "h")) return "source";
    if (ext_is(name, "txt"))  return "text";
    return "file";
}

/* The sidebar's places, in the same order as the table below. */
static const char *place_icon[] = {
    "place-root", "place-home", "place-pics",
    "place-bin",  "place-usb",  "place-tmp",
};

static const struct { const char *label, *path; } places[] = {
    { "Root",      "/"      },
    { "Documents", "/home"  },
    { "Pictures",  "/pics"  },
    { "Programs",  "/bin"   },
    { "USB stick", "/usb"   },
    { "Temp",      "/tmp"   },
};
#define NPLACES ((int)(sizeof places / sizeof places[0]))

/* A folder drawn as a folder, not as the letter D. Two rectangles and a tab;
 * at 16 pixels that is all the detail that survives anyway. */
static void icon_folder(gui_t *g, int x, int y, int s) {
    unsigned body = 0xE9B252, edge = 0xC28F32;
    gui_fill(g, x, y + s / 4, s, s - s / 4, body);
    gui_fill(g, x, y + s / 8, s / 2, s / 4, body);
    gui_rect(g, x, y + s / 4, s, s - s / 4, edge);
    gui_fill(g, x + 1, y + s / 4 + 1, s - 2, 2, 0xF6D08A);
}

static void icon_file(gui_t *g, int x, int y, int s, unsigned tint) {
    int w = s * 3 / 4;
    gui_fill(g, x + (s - w) / 2, y, w, s, GC_PANEL);
    gui_rect(g, x + (s - w) / 2, y, w, s, GC_EDGE);
    gui_fill(g, x + (s - w) / 2 + 2, y + 3, w - 4, 3, tint);
    for (int i = 0; i < 3; i++)
        gui_fill(g, x + (s - w) / 2 + 2, y + 9 + i * 4, w - 5, 1, GC_LINE);
}

static unsigned tint_for(const char *name) {
    if (ext_is(name, "png") || ext_is(name, "jpg") || ext_is(name, "jpeg") ||
        ext_is(name, "bmp")) return 0x5AA469;
    if (ext_is(name, "wav")) return 0xB05AC0;
    if (ext_is(name, "elf")) return 0xC24A2F;
    if (ext_is(name, "c") || ext_is(name, "h") || ext_is(name, "py"))
        return 0x2A6FD6;
    return GC_DIM;
}

static void human(unsigned int n, char *out, int max) {
    if (n < 1024) snprintf(out, max, "%u B", n);
    else if (n < 1024 * 1024) snprintf(out, max, "%u.%u KB", n / 1024, (n % 1024) * 10 / 1024);
    else snprintf(out, max, "%u.%u MB", n / (1024 * 1024),
                  (n % (1024 * 1024)) * 10 / (1024 * 1024));
}

/* Toolbar buttons, laid out left to right. Returned through this table so the
 * click handler and the painter cannot disagree about where they are. */
static const char *btn_label[] = { "Up", "New folder", "Rename", "Delete", "Refresh" };
#define NBTN ((int)(sizeof btn_label / sizeof btn_label[0]))

static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int bw = g->fw * 11, gap = 6;
    *w = bw; *h = TOOL_H - 12;
    *x = 8 + i * (bw + gap);
    *y = 6;
}

static int menu_row_h(gui_t *g) { return g->fh + 10; }
static int menu_w(gui_t *g)     { return g->fw * 14 + 20; }

static void menu_rect(gui_t *g, int *x, int *y, int *w, int *h) {
    *w = menu_w(g);
    *h = NMENU * menu_row_h(g) + 8;
    *x = menu_x;
    *y = menu_y;
    if (*x + *w > g->w) *x = g->w - *w;      /* never open off the window */
    if (*y + *h > g->h) *y = g->h - *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

/* The submenu hangs off the "New file" row, to the right unless it would run
 * off the window, in which case it flips to the left. */
static void sub_rect(gui_t *g, int *x, int *y, int *w, int *h) {
    int mx, my, mw, mh;
    menu_rect(g, &mx, &my, &mw, &mh);
    int rh = menu_row_h(g);
    /* Wide enough for the longest label AND its extension side by side --
     * "Text document" plus ".txt" collide at anything narrower. */
    *w = g->fw * 22 + 20;
    *h = NKINDS * rh + 8;
    *x = mx + mw - 2;
    *y = my + 4 + MENU_NEWFILE * rh - 4;
    if (*x + *w > g->w) *x = mx - *w + 2;
    if (*y + *h > g->h) *y = g->h - *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

static int sub_pick(gui_t *g, int px, int py) {
    int x, y, w, h;
    sub_rect(g, &x, &y, &w, &h);
    if (!gui_in(px, py, x, y, w, h)) return -1;
    int i = (py - y - 4) / menu_row_h(g);
    return (i >= 0 && i < NKINDS) ? i : -1;
}

/* Which item is at this point, or -1. */
static int menu_pick(gui_t *g, int px, int py) {
    int x, y, w, h;
    menu_rect(g, &x, &y, &w, &h);
    if (!gui_in(px, py, x, y, w, h)) return -1;
    int i = (py - y - 4) / menu_row_h(g);
    return (i >= 0 && i < NMENU) ? i : -1;
}

static void draw_menu(gui_t *g) {
    if (!menu_open) return;
    int x, y, w, h;
    menu_rect(g, &x, &y, &w, &h);
    int rh = menu_row_h(g);

    /* A shadow, because a menu that shares an edge colour with the list under
     * it does not read as floating above it. */
    gui_tint(g, x + 3, y + 3, w, h, 0x000000, 60);
    gui_panel(g, x, y, w, h, GC_PANEL, GC_EDGE);

    for (int i = 0; i < NMENU; i++) {
        int iy = y + 4 + i * rh;
        int off = MENU_NEEDS_FILE(i) && sel < 0;
        if (i == menu_hot && !off) gui_fill(g, x + 2, iy, w - 4, rh, GC_SEL);
        gui_text(g, x + 12, iy + (rh - g->fh) / 2, menu_item[i],
                 off ? GC_DIM : GC_TEXT);
        if (i == 2) gui_fill(g, x + 6, iy + rh - 1, w - 12, 1, GC_LINE);

        /* A triangle, because the font has no arrow in it and "there is more
         * this way" has to be visible before the pointer gets there. */
        if (i == MENU_NEWFILE) {
            int ax = x + w - 14, ay = iy + rh / 2;
            for (int k = 0; k < 5; k++)
                gui_fill(g, ax + k, ay - (4 - k), 1, 2 * (4 - k) + 1, GC_DIM);
        }
    }

    if (!sub_open) return;
    int sx, sy, sw, sh;
    sub_rect(g, &sx, &sy, &sw, &sh);
    gui_tint(g, sx + 3, sy + 3, sw, sh, 0x000000, 60);
    gui_panel(g, sx, sy, sw, sh, GC_PANEL, GC_EDGE);
    for (int i = 0; i < NKINDS; i++) {
        int iy = sy + 4 + i * rh;
        if (i == sub_hot) gui_fill(g, sx + 2, iy, sw - 4, rh, GC_SEL);
        gui_text(g, sx + 12, iy + (rh - g->fh) / 2, kinds[i].label, GC_TEXT);
        if (kinds[i].ext[0])
            gui_text(g, sx + sw - 12 - gui_tw(g, kinds[i].ext, 1),
                     iy + (rh - g->fh) / 2, kinds[i].ext, GC_DIM);
    }
}

static void draw(gui_t *g, int blink) {
    gui_clear(g, GC_WIN);

    /* --- sidebar --- */
    gui_vgrad(g, 0, 0, SIDE_W, g->h, GC_BAR, GC_BAR2);
    gui_fill(g, SIDE_W - 1, 0, 1, g->h, GC_EDGE);
    gui_text(g, 12, 12, "Places", GC_DIM);
    for (int i = 0; i < NPLACES; i++) {
        int y = 34 + i * (g->fh + 14);
        int on = strcmp(cwd, places[i].path) == 0;
        if (on) gui_panel(g, 6, y - 4, SIDE_W - 14, g->fh + 10, GC_SEL, GC_ACCENT);
        else if (i == drop_place)
            gui_panel(g, 6, y - 4, SIDE_W - 14, g->fh + 10, GC_HOT, GC_ACCENT);
        int tx = 16;
        if (icon_blit(g, 14, y + (g->fh - ICON_SMALL) / 2, &icons,
                      place_icon[i], ICON_SMALL))
            tx = 14 + ICON_SMALL + 8;
        gui_text(g, tx, y, places[i].label, on ? GC_ACCENT2 : GC_TEXT);
    }

    /* --- toolbar --- */
    gui_vgrad(g, SIDE_W, 0, g->w - SIDE_W, TOOL_H, GC_PANEL, GC_BAR);
    gui_fill(g, SIDE_W, TOOL_H - 1, g->w - SIDE_W, 1, GC_EDGE);
    for (int i = 0; i < NBTN; i++) {
        int x, y, w, h;
        btn_rect(g, i, &x, &y, &w, &h);
        x += SIDE_W;
        int st = gui_in(g->mx, g->my, x, y, w, h) ? GB_HOVER : GB_NORMAL;
        if ((i == 2 || i == 3) && sel < 0) st = GB_OFF;
        gui_button(g, x, y, w, h, btn_label[i], st);
    }

    /* --- path bar --- */
    gui_fill(g, SIDE_W, TOOL_H, g->w - SIDE_W, PATH_H, GC_WIN);
    gui_panel(g, SIDE_W + 8, TOOL_H + 4, g->w - SIDE_W - 16, PATH_H - 8,
              GC_PANEL, GC_EDGE);
    gui_text_clip(g, SIDE_W + 16, TOOL_H + 4 + (PATH_H - 8 - g->fh) / 2, cwd,
                  GC_TEXT, g->w - SIDE_W - 32);

    /* --- list --- */
    int lx, ly, lw, lh;
    list_rect(g, &lx, &ly, &lw, &lh);
    gui_fill(g, lx, ly, lw, lh, GC_PANEL);

    int rh = row_h(g), vis = visible_rows(g);
    int need_sb = count > vis;
    int inner = lw - (need_sb ? SBW : 0);

    for (int i = 0; i < vis && top + i < count; i++) {
        int idx = top + i, y = ly + i * rh;
        unsigned bg = (idx & 1) ? GC_ALT : GC_PANEL;
        if (idx == hover) bg = GC_HOT;
        if (idx == sel)   bg = GC_SEL;
        gui_fill(g, lx, y, inner, rh, bg);
        if (idx == sel) gui_fill(g, lx, y, 3, rh, GC_ACCENT);
        if (idx == drop_row) {          /* where the drag would land */
            gui_fill(g, lx, y, inner, rh, GC_HOT);
            gui_rect(g, lx, y, inner, rh, GC_ACCENT);
        }

        int is = g->fh + 2;
        int iy = y + (rh - ICON_SMALL) / 2;
        if (!icon_blit(g, lx + 12, iy, &icons,
                       icon_for(ents[idx].name, ents[idx].is_dir), ICON_SMALL)) {
            if (ents[idx].is_dir) icon_folder(g, lx + 12, y + (rh - is) / 2, is);
            else icon_file(g, lx + 12, y + (rh - is) / 2, is, tint_for(ents[idx].name));
        }

        int tx = lx + 12 + is + 10, ty = y + (rh - g->fh) / 2;
        int sizew = g->fw * 10;
        gui_text_clip(g, tx, ty, ents[idx].name, GC_TEXT,
                      inner - (tx - lx) - sizew - 20);
        if (!ents[idx].is_dir) {
            char sz[24];
            human(ents[idx].size, sz, sizeof sz);
            gui_text(g, lx + inner - 12 - gui_tw(g, sz, 1), ty, sz, GC_DIM);
        } else {
            gui_text(g, lx + inner - 12 - gui_tw(g, "folder", 1), ty, "folder", GC_DIM);
        }
    }
    if (!count)
        gui_text(g, lx + 20, ly + 20, "This folder is empty.", GC_DIM);
    /* Dropping on the empty part of the list means "into this folder", which
     * is what makes dragging between two windows work at all. A hairline
     * outline is not enough feedback across a whole window -- tint it, so it
     * is obvious at a glance which of two side-by-side windows will take it. */
    if (drop_bg) {
        int iw = lw - (need_sb ? SBW : 0);
        gui_tint(g, lx, ly, iw, lh, GC_ACCENT, 26);
        gui_rect(g, lx, ly, iw, lh, GC_ACCENT);
        gui_rect(g, lx + 1, ly + 1, iw - 2, lh - 2, GC_ACCENT);
    }
    if (need_sb) gui_scrollbar(g, lx + lw - SBW, ly, SBW, lh, top, vis, count);

    /* --- status bar --- */
    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    gui_text_clip(g, 10, g->h - STAT_H + (STAT_H - g->fh) / 2, status,
                  GC_TEXT, g->w - 20);

    draw_menu(g);

    /* --- modal dialog --- */
    if (mode != M_NONE) {
        gui_tint(g, 0, 0, g->w, g->h, 0x000000, 90);
        int dw = g->fw * 40, dh = 130;
        if (dw > g->w - 40) dw = g->w - 40;
        int dx = (g->w - dw) / 2, dy = (g->h - dh) / 2;
        gui_panel(g, dx, dy, dw, dh, GC_WIN, GC_ACCENT);
        gui_vgrad(g, dx + 1, dy + 1, dw - 2, 30, GC_ACCENT, GC_ACCENT2);
        const char *title = mode == M_RENAME  ? "Rename" :
                            mode == M_MKDIR   ? "New folder" :
                            mode == M_NEWFILE ? "New file" : "Delete";
        gui_text(g, dx + 12, dy + 8, title, 0xFFFFFF);

        if (mode == M_DELETE) {
            char q[160];
            snprintf(q, sizeof q, "Delete \"%s\"?",
                     sel >= 0 ? ents[sel].name : "");
            gui_text_clip(g, dx + 14, dy + 46, q, GC_TEXT, dw - 28);
            gui_text(g, dx + 14, dy + 46 + g->fh + 6,
                     "This cannot be undone.", GC_DIM);
        } else {
            gui_edit_draw(g, &editor, dx + 14, dy + 44, dw - 28, g->fh + 14,
                          1, blink, "name");
        }
        int bw = g->fw * 9, bh = g->fh + 12;
        int by = dy + dh - bh - 12;
        gui_button(g, dx + dw - 2 * bw - 22, by, bw, bh, "OK",
                   gui_in(g->mx, g->my, dx + dw - 2 * bw - 22, by, bw, bh)
                       ? GB_HOVER : GB_NORMAL);
        gui_button(g, dx + dw - bw - 12, by, bw, bh, "Cancel",
                   gui_in(g->mx, g->my, dx + dw - bw - 12, by, bw, bh)
                       ? GB_HOVER : GB_NORMAL);
    }
}

/* --- actions -------------------------------------------------------------- */

/* A name nothing in this folder is using yet: new.c, then new1.c, new2.c. */
static void unique_name(const char *ext, char *out, int max) {
    for (int n = 0; n < 100; n++) {
        if (n == 0) snprintf(out, max, "new%s", ext);
        else snprintf(out, max, "new%d%s", n, ext);
        int taken = 0;
        for (int i = 0; i < count; i++)
            if (strcmp(ents[i].name, out) == 0) { taken = 1; break; }
        if (!taken) return;
    }
}

/* MY_FILE_H from my-file.h: uppercase, and anything that is not a letter or a
 * digit becomes an underscore, which is what makes it a legal identifier. */
static void guard_from(const char *name, char *out, int max) {
    int o = 0;
    for (int i = 0; name[i] && o < max - 2; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
        out[o++] = c;
    }
    out[o] = 0;
}

static void write_template(const char *path, const char *name, int kind) {
    char buf[512];
    const char *text = kinds[kind].tmpl;
    if (!text) {                              /* the header guard */
        char g[NAMELEN];
        guard_from(name, g, sizeof g);
        snprintf(buf, sizeof buf,
                 "#ifndef %s\n#define %s\n\n\n\n#endif\n", g, g);
        text = buf;
    }
    if (!text[0]) return;
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, text, strlen(text));
    close(fd);
}

static void begin_newfile(int kind) {
    new_kind = kind;
    char name[NAMELEN];
    unique_name(kinds[kind].ext, name, sizeof name);
    gui_edit_set(&editor, name);
    /* Put the caret where the name ends and the extension begins, so typing
     * replaces what was suggested instead of appending to it. */
    for (int i = 0; name[i]; i++) if (name[i] == '.') { editor.cur = i; break; }
    mode = M_NEWFILE;
}

static void begin_rename(void) {
    if (sel < 0) return;
    gui_edit_set(&editor, ents[sel].name);
    mode = M_RENAME;
}

static void begin_mkdir(void) {
    gui_edit_set(&editor, "New folder");
    mode = M_MKDIR;
}

static void commit(void) {
    char a[UPATH_MAX], b[UPATH_MAX];
    if (mode == M_MKDIR) {
        if (editor.len) {
            upath_resolve(cwd, editor.buf, a);
            if (xyuos_mkdir(a) == 0) snprintf(status, sizeof status, "created %s", editor.buf);
            else snprintf(status, sizeof status, "cannot create %s", editor.buf);
            load();
        }
    } else if (mode == M_NEWFILE) {
        if (editor.len) {
            upath_resolve(cwd, editor.buf, a);
            if (xyuos_create(a) == 0) {
                write_template(a, editor.buf, new_kind);
                snprintf(status, sizeof status, "created %s", editor.buf);
            } else {
                snprintf(status, sizeof status, "cannot create %s", editor.buf);
            }
            char made[NAMELEN];
            snprintf(made, sizeof made, "%s", editor.buf);
            load();
            for (int i = 0; i < count; i++)
                if (strcmp(ents[i].name, made) == 0) { sel = i; break; }
        }
    } else if (mode == M_RENAME && sel >= 0) {
        if (editor.len && strcmp(editor.buf, ents[sel].name) != 0) {
            upath_resolve(cwd, ents[sel].name, a);
            upath_resolve(cwd, editor.buf, b);
            if (xyuos_rename(a, b) == 0) snprintf(status, sizeof status, "renamed");
            else snprintf(status, sizeof status, "rename failed");
            load();
        }
    } else if (mode == M_DELETE && sel >= 0) {
        int keep = sel;
        upath_resolve(cwd, ents[sel].name, a);
        if (xyuos_unlink(a) == 0) snprintf(status, sizeof status, "deleted");
        else snprintf(status, sizeof status, "delete failed (folder not empty?)");
        load();
        if (keep < count) sel = keep;
        else if (count) sel = count - 1;
    }
    mode = M_NONE;
}

static void menu_action(gui_t *g, int i) {
    (void)g;
    menu_open = 0;
    if (MENU_NEEDS_FILE(i) && sel < 0) return;
    switch (i) {
        case 0: activate(sel); break;
        case 1: begin_rename(); break;
        case 2: mode = M_DELETE; break;
        case 4: begin_mkdir(); break;
        case 5: load(); break;
        default: break;                  /* 3 opens the submenu, never acts */
    }
}

static void do_button(gui_t *g, int i) {
    (void)g;
    switch (i) {
        case 0: go(".."); break;
        case 1: begin_mkdir(); break;
        case 2: begin_rename(); break;
        case 3: if (sel >= 0) mode = M_DELETE; break;
        case 4: load(); break;
        default: break;
    }
}

/* --- main ----------------------------------------------------------------- */

int main(int argc, char **argv) {
    gui_t g;
    if (argc > 1) snprintf(cwd, sizeof cwd, "%s", argv[1]);
    if (!gui_open(&g)) return 1;
    icons_open(&icons);          /* decoration: a failure here changes nothing */
    load();

    unsigned last_click = 0;
    int last_idx = -1;
    int running = 1, dirty = 1, dragging_sb = 0, last_blink = -1;
    int hover_btn = -2;

    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type == GE_KEY) {
                dirty = 1;
                key_event_t *k = &e.k;
                if (k->code == XKEY_RESIZE) { gui_sync(&g); continue; }

                if (mode != M_NONE) {
                    if (k->code == XKEY_ESC)   { mode = M_NONE; continue; }
                    if (k->code == XKEY_ENTER) { commit(); continue; }
                    if (mode != M_DELETE) gui_edit_key(&editor, k);
                    continue;
                }
                if (menu_open && k->code == XKEY_ESC) {
                    if (sub_open) sub_open = 0;
                    else menu_open = 0;
                    continue;
                }

                switch (k->code) {
                    case XKEY_UP:    if (sel > 0) sel--; scroll_to_sel(&g); break;
                    case XKEY_DOWN:  if (sel < count - 1) sel++; scroll_to_sel(&g); break;
                    case XKEY_HOME:  sel = count ? 0 : -1; scroll_to_sel(&g); break;
                    case XKEY_END:   sel = count - 1; scroll_to_sel(&g); break;
                    case XKEY_PGUP:  sel -= visible_rows(&g); if (sel < 0) sel = 0;
                                     scroll_to_sel(&g); break;
                    case XKEY_PGDN:  sel += visible_rows(&g);
                                     if (sel > count - 1) sel = count - 1;
                                     scroll_to_sel(&g); break;
                    case XKEY_ENTER: activate(sel); break;
                    case XKEY_BKSP:  go(".."); break;
                    case XKEY_DEL:   if (sel >= 0) mode = M_DELETE; break;
                    case XKEY_ESC:   running = 0; break;
                    default:
                        if (k->code == XKEY_F(2)) begin_rename();
                        else if (k->code == XKEY_F(5)) load();
                        else if (k->code == XKEY_CHAR && k->ascii == 'q') running = 0;
                        break;
                }
                continue;
            }

            /* --- pointer --- */
            mouse_event_t *m = &e.m;

            /* Something is being dragged across this window. Work out where
             * it would land and show it; on release, do the move. */
            if (m->drag == DRAG_LEAVE) {
                if (drop_row >= 0 || drop_place >= 0 || drop_bg) dirty = 1;
                drop_row = drop_place = -1;
                drop_bg = 0;
                continue;
            }
            if (m->drag) {
                int rx, ry, rw, rh2;
                list_rect(&g, &rx, &ry, &rw, &rh2);
                int was_r = drop_row, was_p = drop_place, was_b = drop_bg;
                drop_row = drop_place = -1;
                drop_bg = 0;

                if (gui_in(m->x, m->y, rx, ry, rw - SBW, rh2)) {
                    int i = top + (m->y - ry) / row_h(&g);
                    if (i >= 0 && i < count && ents[i].is_dir) drop_row = i;
                    else drop_bg = 1;
                } else {
                    for (int i = 0; i < NPLACES; i++) {
                        int y = 34 + i * (g.fh + 14);
                        if (gui_in(m->x, m->y, 6, y - 4, SIDE_W - 14, g.fh + 10))
                            drop_place = i;
                    }
                }
                if (drop_row != was_r || drop_place != was_p || drop_bg != was_b)
                    dirty = 1;

                if (m->drag == DRAG_DROP) {
                    char src[UPATH_MAX];
                    int n = drag_take(src, sizeof src);
                    if (n > 0) {
                        char dstdir[UPATH_MAX];
                        if (drop_row >= 0) upath_resolve(cwd, ents[drop_row].name, dstdir);
                        else if (drop_place >= 0)
                            snprintf(dstdir, sizeof dstdir, "%s", places[drop_place].path);
                        else snprintf(dstdir, sizeof dstdir, "%s", cwd);
                        move_into(src, dstdir);
                    }
                    drop_row = drop_place = -1;
                    drop_bg = 0;
                    may_drag = 0;
                    dirty = 1;
                }
                continue;
            }

            /* The right button opens the menu on whatever it landed on. */
            if (m->pressed & MB_RIGHT) {
                int rx, ry, rw, rh2;
                list_rect(&g, &rx, &ry, &rw, &rh2);
                if (gui_in(m->x, m->y, rx, ry, rw - SBW, rh2)) {
                    int i = top + (m->y - ry) / row_h(&g);
                    sel = (i >= 0 && i < count) ? i : -1;
                }
                menu_x = m->x;
                menu_y = m->y;
                menu_hot = -1;
                sub_open = 0;
                sub_hot = -1;
                menu_open = 1;
                dirty = 1;
                continue;
            }

            if (menu_open) {
                int hot = menu_pick(&g, m->x, m->y);
                int shot = sub_open ? sub_pick(&g, m->x, m->y) : -1;

                /* Hovering the parent row opens the submenu; hovering any
                 * other row closes it again. Moving diagonally onto the
                 * submenu itself must not count as leaving. */
                if (hot == MENU_NEWFILE && !sub_open) { sub_open = 1; dirty = 1; }
                else if (hot >= 0 && hot != MENU_NEWFILE && sub_open && shot < 0) {
                    sub_open = 0;
                    dirty = 1;
                }
                if (hot != menu_hot)  { menu_hot = hot;  dirty = 1; }
                if (shot != sub_hot)  { sub_hot = shot;  dirty = 1; }

                if (m->pressed & MB_LEFT) {
                    if (shot >= 0) {
                        menu_open = sub_open = 0;
                        begin_newfile(shot);
                    } else if (hot == MENU_NEWFILE) {
                        sub_open = 1;
                    } else if (hot >= 0) {
                        menu_action(&g, hot);
                    } else {
                        menu_open = sub_open = 0;   /* a click outside dismisses */
                    }
                    dirty = 1;
                }
                continue;                    /* modal: nothing else sees this */
            }

            int lx, ly, lw, lh;
            list_rect(&g, &lx, &ly, &lw, &lh);
            int rh = row_h(&g), vis = visible_rows(&g);

            if (mode != M_NONE) {
                int dw = g.fw * 40, dh = 130;
                if (dw > g.w - 40) dw = g.w - 40;
                int dx = (g.w - dw) / 2, dy = (g.h - dh) / 2;
                int bw = g.fw * 9, bh = g.fh + 12, by = dy + dh - bh - 12;
                if (gui_clicked(&e, dx + dw - 2 * bw - 22, by, bw, bh)) commit();
                else if (gui_clicked(&e, dx + dw - bw - 12, by, bw, bh)) mode = M_NONE;
                continue;
            }

            /* hover feedback -- and only repaint when it actually moved to a
             * different row or button, not on every pixel of travel. */
            int was_hover = hover;
            hover = -1;
            if (gui_in(m->x, m->y, lx, ly, lw - SBW, lh)) {
                int i = top + (m->y - ly) / rh;
                if (i >= 0 && i < count) hover = i;
            }
            {
                int hb = -1;
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    x += SIDE_W;
                    if (gui_in(m->x, m->y, x, y, w, h)) hb = i;
                }
                if (hb != hover_btn) { hover_btn = hb; dirty = 1; }
            }
            if (hover != was_hover || gui_acts(&e) || dragging_sb) dirty = 1;

            if (m->released & MB_LEFT) { dragging_sb = 0; may_drag = 0; }

            /* Movement with the button down, past a few pixels of slack, is a
             * drag. Below that it is a click with a shaky hand. */
            if (may_drag && (m->buttons & MB_LEFT) && drag_row >= 0) {
                int dx = m->x - press_x, dy = m->y - press_y;
                if (dx * dx + dy * dy > 36) {
                    char full[UPATH_MAX];
                    upath_resolve(cwd, ents[drag_row].name, full);
                    drag_begin(full, ents[drag_row].name);
                    may_drag = 0;
                }
            }

            if (m->wheel) {
                top -= m->wheel * 3;
                if (top > count - vis) top = count - vis;
                if (top < 0) top = 0;
            }

            if (dragging_sb && (m->buttons & MB_LEFT))
                top = gui_scrollbar_pick(ly, lh, m->y, vis, count);

            if (m->pressed & MB_LEFT) {
                /* sidebar */
                for (int i = 0; i < NPLACES; i++) {
                    int y = 34 + i * (g.fh + 14);
                    if (gui_in(m->x, m->y, 6, y - 4, SIDE_W - 14, g.fh + 10))
                        go(places[i].path);
                }
                /* toolbar */
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    x += SIDE_W;
                    if (gui_in(m->x, m->y, x, y, w, h)) do_button(&g, i);
                }
                /* scrollbar */
                if (count > vis && gui_in(m->x, m->y, lx + lw - SBW, ly, SBW, lh)) {
                    dragging_sb = 1;
                    top = gui_scrollbar_pick(ly, lh, m->y, vis, count);
                }
                /* list: select, and open on a double click */
                else if (gui_in(m->x, m->y, lx, ly, lw - SBW, lh)) {
                    int i = top + (m->y - ly) / rh;
                    if (i >= 0 && i < count) {
                        may_drag = 1;
                        drag_row = i;
                        press_x = m->x;
                        press_y = m->y;
                        unsigned now = uptime_ms();
                        if (i == last_idx && now - last_click < 450) {
                            sel = i;
                            activate(i);
                            last_idx = -1;
                        } else {
                            sel = i;
                            last_idx = i;
                            last_click = now;
                        }
                    }
                }
            }
        }

        if (dirty) {
            if (gui_sync(&g)) {
                draw(&g, (int)((uptime_ms() / 500) & 1));
                gui_present(&g);
                dirty = 0;
            } else if (gui_lost(&g)) {
                break;          /* the window really is gone */
            }
        }
        /* Only a dialog has a blinking caret, and only twice a second. */
        sleep_ms(40);
        if (mode != M_NONE) {
            int phase = (int)((uptime_ms() / 500) & 1);
            if (phase != last_blink) { last_blink = phase; dirty = 1; }
        }
    }

    gui_close(&g);
    return 0;
}
