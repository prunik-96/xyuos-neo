/* Measuring text, and the one function that decides how big it is.
 *
 * The font here is a bitmap one at a fixed cell, magnified by a whole number.
 * That sounds like a limitation and is mostly a gift: measurement is exact,
 * costs nothing, and cannot disagree with drawing, because both go through
 * xy_font_scale below. Half the awkward bugs in a browser's text layout come
 * from measuring with one set of metrics and drawing with another.
 *
 * What it does cost is choice. A page asking for 11pt and one asking for 12pt
 * get the same size when both round to the same magnification. Between 1x and
 * 2x that is a wide gap, and it is the honest consequence of a font that
 * exists at one size.
 */

#include <string.h>

#include "utils/log.h"
#include "netsurf/layout.h"
#include "netsurf/plot_style.h"

#include "xy_front.h"

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

static nserror xyl_width(const plot_font_style_t *fstyle,
                         const char *string, size_t length, int *width) {
    *width = chars_in(string, length) * xy_gui.fw * xy_font_scale(fstyle);
    return NSERROR_OK;
}

static nserror xyl_position(const plot_font_style_t *fstyle,
                            const char *string, size_t length, int x,
                            size_t *char_offset, int *actual_x) {
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
