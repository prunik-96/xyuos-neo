/* Measuring text, and the one place a NetSurf font becomes a real one.
 *
 * With the fonts on the disk, every question NetSurf asks -- how wide is
 * this, where did the click land, where should this line end -- goes to
 * libtext, which answers from the same shaping it draws from. Kerning,
 * ligatures, Arabic joining and the rest therefore reach the layout, not
 * only the pixels: a line of Arabic is exactly as wide as it will be drawn.
 *
 * Without them the old bitmap font is still here: fixed cells, magnified by
 * a whole number. Measurement is trivially exact there too, because both
 * sides go through xy_font_scale. It is the fallback, not the design.
 */

#include <string.h>
#include <strings.h>

#include <libwapcaplet/libwapcaplet.h>

#include "utils/log.h"
#include "netsurf/layout.h"
#include "netsurf/plot_style.h"
#include "netsurf/browser_window.h"

#include "xy_front.h"

bool xy_text = false;

void xy_text_init(void) {
    xy_text = txt_init(NULL) > 0;
    if (!xy_text)
        NSLOG(netsurf, WARNING, "no fonts in /fonts: using the bitmap font");
}

/* --- which family ------------------------------------------------------- */

static int has(const char *s, const char *what) { return strstr(s, what) != NULL; }

/* A page names fonts it hopes the reader has. We have three, so each name is
 * sorted into the one it most resembles, and a name that says nothing about
 * that is passed over for the next in the list. */
static int family_named(lwc_string *name) {
    char s[64];
    size_t n = lwc_string_length(name);
    if (n >= sizeof s) n = sizeof s - 1;
    for (size_t i = 0; i < n; i++) {
        char c = lwc_string_data(name)[i];
        s[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    s[n] = 0;

    if (has(s, "mono") || has(s, "courier") || has(s, "consol") ||
        has(s, "menlo") || has(s, "monaco") || has(s, "code") ||
        strcmp(s, "monospace") == 0)
        return TXT_MONO;
    if ((has(s, "serif") && !has(s, "sans")) || has(s, "times") ||
        has(s, "georgia") || has(s, "garamond") || has(s, "cambria") ||
        has(s, "palatino") || has(s, "baskerville") || has(s, "libertine") ||
        has(s, "merriweather") || has(s, "book"))
        return TXT_SERIF;
    if (has(s, "sans") || has(s, "arial") || has(s, "helvetica") ||
        has(s, "verdana") || has(s, "tahoma") || has(s, "segoe") ||
        has(s, "roboto") || has(s, "system-ui") || has(s, "apple-system") ||
        has(s, "inter") || has(s, "ubuntu") || has(s, "lato") ||
        has(s, "open sans") || has(s, "noto"))
        return TXT_SANS;
    return -1;
}

void xy_text_style(const plot_font_style_t *f, txt_style *st) {
    int fam = -1;
    if (f->families)
        for (lwc_string * const *p = f->families; *p && fam < 0; p++)
            fam = family_named(*p);
    if (fam < 0)
        fam = f->family == PLOT_FONT_FAMILY_SERIF     ? TXT_SERIF
            : f->family == PLOT_FONT_FAMILY_MONOSPACE ? TXT_MONO
            :                                           TXT_SANS;
    st->family = (txt_family)fam;
    st->weight = f->weight;
    st->italic = (f->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0;

    /* NetSurf hands over points, fixed point with ten fraction bits, having
     * turned CSS pixels into them at browser_get_dpi(). Back to pixels at
     * the same rate, so 16px in the style sheet is 16 pixels here. */
    long long v = (long long)f->size * browser_get_dpi() * 64 /
                  (72LL << PLOT_STYLE_RADIX);
    if (v < 64) v = 64;
    if (v > 512 * 64) v = 512 * 64;
    st->size = (int)v;
}

/* --- the bitmap font, for when there are no fonts ------------------------ */

/* Points to pixels at the 96dpi CSS assumes, then to whole cells. */
int xy_font_scale(const plot_font_style_t *fstyle) {
    int pt = plot_style_fixed_to_int(fstyle->size);
    if (pt <= 0) pt = 12;
    int px = pt * 4 / 3;
    int cell = xy_gui.fh > 0 ? xy_gui.fh : 8;
    int s = (px + cell / 2) / cell;
    if (s < 1) s = 1;
    if (s > 6) s = 6;          /* beyond this a heading is a wall, not a word */
    return s;
}

/* Where the baseline falls inside the cell. The glyphs in this font sit on
 * the seventh row of eight, so the descender has the bottom one. */
int xy_font_ascent(int scale) {
    int cell = xy_gui.fh > 0 ? xy_gui.fh : 8;
    return (cell * 7 / 8) * scale;
}

/* Characters, not bytes: the string is UTF-8 and the cell is per character. */
static int chars_in(const char *s, size_t len) {
    int n = 0;
    for (size_t i = 0; i < len && s[i]; n++) {
        int cp;
        i += (size_t)gui_utf8(s + i, &cp);
    }
    return n;
}

/* --- the three questions -------------------------------------------------- */

static nserror xyl_width(const plot_font_style_t *fstyle,
                         const char *string, size_t length, int *width) {
    if (xy_text) {
        txt_style st;
        xy_text_style(fstyle, &st);
        *width = txt_width(&st, string, length);
        return NSERROR_OK;
    }
    *width = chars_in(string, length) * xy_gui.fw * xy_font_scale(fstyle);
    return NSERROR_OK;
}

static nserror xyl_position(const plot_font_style_t *fstyle,
                            const char *string, size_t length, int x,
                            size_t *char_offset, int *actual_x) {
    if (xy_text) {
        txt_style st;
        xy_text_style(fstyle, &st);
        *char_offset = txt_hit(&st, string, length, x, actual_x);
        return NSERROR_OK;
    }

    int step = xy_gui.fw * xy_font_scale(fstyle);
    if (step < 1) step = 1;

    /* Walk the string rather than dividing: the answer is a byte offset, and
     * a multi-byte character has more bytes than it has width. */
    size_t i = 0;
    int at = 0;
    while (i < length && string[i]) {
        if (at + step > x) break;
        int cp;
        i += (size_t)gui_utf8(string + i, &cp);
        at += step;
    }
    *char_offset = i;
    *actual_x = at;
    return NSERROR_OK;
}

static nserror xyl_split(const plot_font_style_t *fstyle,
                         const char *string, size_t length, int x,
                         size_t *char_offset, int *actual_x) {
    if (xy_text) {
        /* Where a line may end is Unicode's to say (UAX #14), not only at
         * spaces: after a hyphen, between two Chinese characters. NetSurf's
         * layout drops the space at a split point and keeps anything else. */
        txt_style st;
        xy_text_style(fstyle, &st);
        *char_offset = txt_split(&st, string, length, x, actual_x);
        return NSERROR_OK;
    }

    int step = xy_gui.fw * xy_font_scale(fstyle);
    if (step < 1) step = 1;

    /* The last space that still fits. NetSurf breaks lines here, so the
     * answer has to be a word boundary and never zero -- a zero would mean
     * "split before the first character", and the line would never advance. */
    size_t i = 0, lastsp = 0;
    int at = 0, lastx = 0;
    bool found = false;

    while (i < length && string[i]) {
        if (string[i] == ' ' && i > 0) {
            if (at > x && found) break;    /* past the width, keep the last */
            lastsp = i;
            lastx = at;
            found = true;
        }
        int cp;
        i += (size_t)gui_utf8(string + i, &cp);
        at += step;
    }

    if (found) {
        *char_offset = lastsp;
        *actual_x = lastx;
    } else {
        /* One long word: it does not fit and cannot be broken. Saying so is
         * what makes it overflow rather than disappear. */
        *char_offset = length;
        *actual_x = at;
    }
    return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
    .width    = xyl_width,
    .position = xyl_position,
    .split    = xyl_split,
};

struct gui_layout_table *xy_layout_table = &layout_table;
