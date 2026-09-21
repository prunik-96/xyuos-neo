/* xyuOS Neo text editor.
 *
 * Started as `edit FILE`, owns the pane until Ctrl+Q.
 *
 *   arrows          move            shift+arrows   select
 *   Tab             indent          Ctrl+A         select all
 *   Ctrl+C/X/V      copy/cut/paste  Ctrl+Z         undo
 *   Ctrl+S          save            Ctrl+Y         redo
 *   Ctrl+Q          quit
 *
 * Two things about the display are worth knowing:
 *
 * - Tabs. The pane renders '\t' as four spaces, so any column arithmetic that
 *   counts it as one character puts the cursor in the wrong place. That was
 *   the old editor's bug. Here every column is a VISUAL column (see vcol_of),
 *   and pressing Tab inserts spaces rather than a tab, so files this editor
 *   writes have no ambiguity in them at all.
 *
 * - Colors. The pane has 8 palette slots and no escape sequences: color is an
 *   attribute set before the text is written. So highlighting means splitting
 *   each line into runs of one color and writing them one run at a time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <tty.h>
#include "upath.h"

#define EDIT_MAX   65536
#define CLIP_MAX    8192
#define TAB_WIDTH      4
#define GUTTER         5      /* "%4d " */
#define UNDO_MAX     128
#define OP_TEXT_MAX   64

static char *text;
static int   len, cur, top_line;
static int   dirty, saved;
static char  path[UPATH_MAX];
static char  msg[80];          /* transient status message */

static int   sel_anchor = -1;  /* -1: no selection */

static char  clip[CLIP_MAX];
static int   clip_len;

/* --- undo ---------------------------------------------------------------
 * Operation based rather than snapshot based: a 64 KiB buffer times a useful
 * history depth would not fit. Consecutive single-character inserts at
 * adjacent positions coalesce into one entry, so undo steps back by words
 * rather than by keystrokes. */

#define OP_INSERT 1
#define OP_DELETE 2

struct op {
    int  type;
    int  pos;
    int  len;
    /* Only an "open" op accepts more characters. A paste, a cut, or anything
     * that moved the cursor closes the current one, so undo never lumps a
     * paste together with the typing that happened to follow it. */
    int  open;
    char data[OP_TEXT_MAX];
};

static struct op undo_stack[UNDO_MAX], redo_stack[UNDO_MAX];
static int undo_top, redo_top;

static void push_op(struct op *stack, int *top, int type, int pos,
                    const char *data, int n, int open) {
    if (n > OP_TEXT_MAX) n = OP_TEXT_MAX;
    if (*top == UNDO_MAX) {                       /* drop the oldest */
        for (int i = 1; i < UNDO_MAX; i++) stack[i - 1] = stack[i];
        (*top)--;
    }
    struct op *o = &stack[(*top)++];
    o->type = type;
    o->pos = pos;
    o->len = n;
    o->open = open;
    for (int i = 0; i < n; i++) o->data[i] = data[i];
}

/* Stop the current op from absorbing anything further. Called whenever the
 * cursor moves or a non-typing edit happens. */
static void close_op(void) {
    if (undo_top > 0) undo_stack[undo_top - 1].open = 0;
}

/* Record an edit for undo. `coalesce` merges consecutive typing. */
static void record(int type, int pos, const char *data, int n, int coalesce) {
    redo_top = 0;                                 /* a new edit forks history */

    if (coalesce && undo_top > 0) {
        struct op *last = &undo_stack[undo_top - 1];
        if (last->open && last->type == type && last->len + n <= OP_TEXT_MAX) {
            if (type == OP_INSERT && last->pos + last->len == pos) {
                for (int i = 0; i < n; i++) last->data[last->len + i] = data[i];
                last->len += n;
                return;
            }
            if (type == OP_DELETE && pos + n == last->pos) {   /* backspacing */
                for (int i = last->len - 1; i >= 0; i--)
                    last->data[i + n] = last->data[i];
                for (int i = 0; i < n; i++) last->data[i] = data[i];
                last->pos = pos;
                last->len += n;
                return;
            }
        }
    }
    push_op(undo_stack, &undo_top, type, pos, data, n, coalesce);
}

/* --- raw buffer edits (these do NOT record undo) ------------------------- */

static void raw_insert(int pos, const char *s, int n) {
    if (len + n > EDIT_MAX - 1) n = EDIT_MAX - 1 - len;
    if (n <= 0) return;
    memmove(&text[pos + n], &text[pos], len - pos);
    for (int i = 0; i < n; i++) text[pos + i] = s[i];
    len += n;
}

static void raw_delete(int pos, int n) {
    if (pos + n > len) n = len - pos;
    if (n <= 0) return;
    memmove(&text[pos], &text[pos + n], len - pos - n);
    len -= n;
}

/* --- edits that DO record undo ------------------------------------------- */

static void do_insert(int pos, const char *s, int n, int coalesce) {
    raw_insert(pos, s, n);
    record(OP_INSERT, pos, s, n, coalesce);
    dirty = 1; saved = 0;
}

static void do_delete(int pos, int n, int coalesce) {
    if (n <= 0) return;
    record(OP_DELETE, pos, &text[pos], n, coalesce);
    raw_delete(pos, n);
    dirty = 1; saved = 0;
}

/* An op always describes what HAPPENED. Undo runs it backwards, redo runs it
 * forwards, and the op itself is moved between the two stacks unchanged --
 * flipping its type as well would invert twice and cancel out. */

static void apply_backward(const struct op *o) {
    if (o->type == OP_INSERT) {            /* it inserted, so remove it again */
        raw_delete(o->pos, o->len);
        cur = o->pos;
    } else {                               /* it deleted, so put it back */
        raw_insert(o->pos, o->data, o->len);
        cur = o->pos + o->len;
    }
    dirty = 1; saved = 0;
}

static void apply_forward(const struct op *o) {
    if (o->type == OP_INSERT) {
        raw_insert(o->pos, o->data, o->len);
        cur = o->pos + o->len;
    } else {
        raw_delete(o->pos, o->len);
        cur = o->pos;
    }
    dirty = 1; saved = 0;
}

static void undo(void) {
    if (undo_top == 0) { snprintf(msg, sizeof(msg), "nothing to undo"); return; }
    struct op o = undo_stack[--undo_top];
    apply_backward(&o);
    push_op(redo_stack, &redo_top, o.type, o.pos, o.data, o.len, 0);
    sel_anchor = -1;
}

static void redo(void) {
    if (redo_top == 0) { snprintf(msg, sizeof(msg), "nothing to redo"); return; }
    struct op o = redo_stack[--redo_top];
    apply_forward(&o);
    push_op(undo_stack, &undo_top, o.type, o.pos, o.data, o.len, 0);
    sel_anchor = -1;
}

/* --- file --------------------------------------------------------------- */

static void load(const char *p) {
    snprintf(path, sizeof(path), "%s", p);
    len = cur = top_line = dirty = saved = 0;

    int fd = open(path, 0);
    if (fd < 0) { snprintf(msg, sizeof(msg), "new file"); return; }
    long got;
    while ((got = read(fd, text + len, EDIT_MAX - 1 - len)) > 0) {
        len += (int)got;
        if (len >= EDIT_MAX - 1) break;
    }
    close(fd);
}

static void save_file(void) {
    /* No truncate-on-open in this VFS, so replace the file wholesale. */
    xyuos_unlink(path);
    if (xyuos_create(path) != 0) {
        snprintf(msg, sizeof(msg), "SAVE FAILED (cannot create)");
        return;
    }
    int fd = open(path, 0);
    if (fd < 0) { snprintf(msg, sizeof(msg), "SAVE FAILED (cannot open)"); return; }
    if (len > 0) write(fd, text, len);
    close(fd);
    dirty = 0; saved = 1;
    snprintf(msg, sizeof(msg), "saved %d bytes", len);
}

/* --- positions ----------------------------------------------------------- */

static int line_start(int line) {
    if (line <= 0) return 0;
    int ln = 0;
    for (int i = 0; i < len; i++)
        if (text[i] == '\n' && ++ln == line) return i + 1;
    return len;
}

static int line_end(int start) {
    int i = start;
    while (i < len && text[i] != '\n') i++;
    return i;
}

static int line_of(int off) {
    int ln = 0;
    for (int i = 0; i < off && i < len; i++) if (text[i] == '\n') ln++;
    return ln;
}

/* Visual column of `off`, expanding tabs -- the whole point of which is that
 * the cursor lands where the text actually is. */
static int vcol_of(int off) {
    int start = line_start(line_of(off));
    int v = 0;
    for (int i = start; i < off && i < len; i++) {
        if (text[i] == '\t') v += TAB_WIDTH - (v % TAB_WIDTH);
        else v++;
    }
    return v;
}

/* Offset on `line` nearest to visual column `want` -- used by up/down so the
 * cursor keeps its column across lines of different indentation. */
static int off_at_vcol(int line, int want) {
    int start = line_start(line);
    int end = line_end(start);
    int v = 0;
    for (int i = start; i < end; i++) {
        if (v >= want) return i;
        if (text[i] == '\t') v += TAB_WIDTH - (v % TAB_WIDTH);
        else v++;
    }
    return end;
}

static int total_lines(void) {
    int n = 1;
    for (int i = 0; i < len; i++) if (text[i] == '\n') n++;
    return n;
}

/* --- selection ----------------------------------------------------------- */

static int sel_start(void) { return (sel_anchor < cur) ? sel_anchor : cur; }
static int sel_end(void)   { return (sel_anchor < cur) ? cur : sel_anchor; }
static int has_sel(void)   { return sel_anchor >= 0 && sel_anchor != cur; }

static void delete_selection(void) {
    if (!has_sel()) return;
    int a = sel_start(), b = sel_end();
    do_delete(a, b - a, 0);
    cur = a;
    sel_anchor = -1;
}

static void copy_selection(int cut) {
    int a, b;
    if (has_sel()) { a = sel_start(); b = sel_end(); }
    else {                                   /* no selection: the whole line */
        a = line_start(line_of(cur));
        b = line_end(a);
        if (b < len) b++;                    /* include the newline */
    }
    clip_len = b - a;
    if (clip_len > CLIP_MAX) clip_len = CLIP_MAX;
    for (int i = 0; i < clip_len; i++) clip[i] = text[a + i];

    if (cut) {
        do_delete(a, clip_len, 0);
        cur = a;
        sel_anchor = -1;
        snprintf(msg, sizeof(msg), "cut %d bytes", clip_len);
    } else {
        snprintf(msg, sizeof(msg), "copied %d bytes", clip_len);
    }
}

static void paste(void) {
    if (clip_len == 0) { snprintf(msg, sizeof(msg), "clipboard empty"); return; }
    delete_selection();
    /* Chunked because one undo entry holds at most OP_TEXT_MAX bytes. */
    int done = 0;
    while (done < clip_len) {
        int n = clip_len - done;
        if (n > OP_TEXT_MAX) n = OP_TEXT_MAX;
        do_insert(cur, clip + done, n, 0);
        cur += n;
        done += n;
    }
    snprintf(msg, sizeof(msg), "pasted %d bytes", clip_len);
}

/* --- syntax highlighting -------------------------------------------------
 *
 * Deliberately lexical, not semantic: it colors what a character IS, never
 * what it means. That keeps it linear, stateless apart from block comments,
 * and impossible to get "wrong" in a way that misleads. */

#define C_TEXT    TC_WHITE
#define C_KEYWORD TC_MAGENTA
#define C_TYPE    TC_CYAN
#define C_STRING  TC_GREEN
#define C_NUMBER  TC_YELLOW
#define C_COMMENT TC_BLUE
#define C_PREPROC TC_MAGENTA

static const char *keywords[] = {
    "if","else","for","while","do","switch","case","default","break","continue",
    "return","goto","sizeof","typedef","static","extern","const","volatile",
    "inline","register","auto","restrict","_Bool", 0
};
static const char *types[] = {
    "int","char","void","long","short","float","double","unsigned","signed",
    "struct","union","enum","size_t","uint8_t","uint16_t","uint32_t","uint64_t",
    "int8_t","int16_t","int32_t","int64_t","FILE","bool", 0
};

static int is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int word_in(const char **list, const char *s, int n) {
    for (int i = 0; list[i]; i++) {
        int k = 0;
        while (list[i][k] && k < n && list[i][k] == s[k]) k++;
        if (k == n && list[i][k] == '\0') return 1;
    }
    return 0;
}

/* Is offset `off` inside a block comment? Scanned from the start of the file;
 * only called once per repaint, for the first visible line. */
static int in_block_comment_at(int off) {
    int in_c = 0, in_s = 0;
    char sq = 0;
    for (int i = 0; i < off && i < len; i++) {
        if (in_c) {
            if (text[i] == '*' && i + 1 < len && text[i + 1] == '/') { in_c = 0; i++; }
            continue;
        }
        if (in_s) {
            if (text[i] == '\\') { i++; continue; }
            if (text[i] == sq) in_s = 0;
            continue;
        }
        if (text[i] == '"' || text[i] == '\'') { in_s = 1; sq = text[i]; continue; }
        if (text[i] == '/' && i + 1 < len) {
            if (text[i + 1] == '*') { in_c = 1; i++; }
            else if (text[i + 1] == '/') { while (i < len && text[i] != '\n') i++; }
        }
    }
    return in_c;
}

/* Color every character of one line. `in_c` carries block-comment state in
 * and out. */
static void highlight(const char *s, int n, int *in_c, unsigned char *col) {
    int i = 0;

    /* A preprocessor directive colors the whole line, except comments. */
    int pp = 0;
    for (int k = 0; k < n; k++) {
        if (s[k] == ' ' || s[k] == '\t') continue;
        pp = (s[k] == '#');
        break;
    }

    while (i < n) {
        if (*in_c) {
            col[i] = C_COMMENT;
            if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                col[i + 1] = C_COMMENT;
                *in_c = 0;
                i += 2;
                continue;
            }
            i++;
            continue;
        }

        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n) col[i++] = C_COMMENT;
            break;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            col[i] = col[i + 1] = C_COMMENT;
            *in_c = 1;
            i += 2;
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i];
            col[i++] = C_STRING;
            while (i < n) {
                col[i] = C_STRING;
                if (s[i] == '\\' && i + 1 < n) { col[++i] = C_STRING; i++; continue; }
                if (s[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (is_digit(s[i]) && (i == 0 || !is_word(s[i - 1]))) {
            while (i < n && (is_word(s[i]) || s[i] == '.')) col[i++] = C_NUMBER;
            continue;
        }
        if (is_word(s[i])) {
            int start = i;
            while (i < n && is_word(s[i])) i++;
            int wl = i - start;
            unsigned char c = pp ? C_PREPROC : C_TEXT;
            if (word_in(keywords, s + start, wl))   c = C_KEYWORD;
            else if (word_in(types, s + start, wl)) c = C_TYPE;
            for (int k = start; k < i; k++) col[k] = c;
            continue;
        }

        col[i] = pp ? C_PREPROC : C_TEXT;
        i++;
    }
}

/* --- drawing ------------------------------------------------------------- */

#define MAXCOLS 240

static void draw(void) {
    int cols, rows;
    tty_size(&cols, &rows);
    if (cols > MAXCOLS) cols = MAXCOLS;

    int text_rows = (rows >= 2) ? rows - 1 : 1;
    int text_cols = cols - GUTTER;
    if (text_cols < 1) text_cols = 1;

    int cline = line_of(cur);
    int cvcol = vcol_of(cur);
    if (cline < top_line) top_line = cline;
    if (cline >= top_line + text_rows) top_line = cline - text_rows + 1;

    tty_reset_color();
    tty_clear();

    int a = has_sel() ? sel_start() : -1;
    int b = has_sel() ? sel_end()   : -1;

    int off = line_start(top_line);
    int in_c = in_block_comment_at(off);
    int nlines = total_lines();

    static unsigned char col[MAXCOLS * 4];
    static char vis[MAXCOLS * 4];
    static int  vis_off[MAXCOLS * 4];

    for (int r = 0; r < text_rows; r++) {
        int lineno = top_line + r;
        tty_move(r, 0);

        if (lineno >= nlines) {                 /* past the end of the file */
            tty_set_color(TC_BLUE, TC_BLACK);
            printf("   ~ ");
            tty_reset_color();
            continue;
        }

        int ls = off;
        int le = line_end(ls);
        int n = le - ls;
        if (n > MAXCOLS * 2) n = MAXCOLS * 2;

        /* gutter */
        tty_set_color(lineno == cline ? TC_YELLOW : TC_BLUE, TC_BLACK);
        printf("%4d ", lineno + 1);
        tty_reset_color();

        /* color the raw line, then expand tabs into the visual line, carrying
           each cell's color and source offset along with it */
        for (int i = 0; i < n; i++) col[i] = C_TEXT;
        int carry = in_c;
        highlight(&text[ls], n, &carry, col);

        int v = 0;
        for (int i = 0; i < n && v < MAXCOLS * 3; i++) {
            if (text[ls + i] == '\t') {
                int stop = TAB_WIDTH - (v % TAB_WIDTH);
                for (int k = 0; k < stop && v < MAXCOLS * 3; k++) {
                    vis[v] = ' '; col[v + MAXCOLS] = col[i]; vis_off[v] = ls + i; v++;
                }
            } else {
                vis[v] = text[ls + i];
                col[v + MAXCOLS] = col[i];
                vis_off[v] = ls + i;
                v++;
            }
        }
        in_c = carry;
        off = (le < len) ? le + 1 : len;

        /* emit runs of identical (color, selected) */
        int shown = (v < text_cols) ? v : text_cols;
        int i = 0;
        while (i < shown) {
            int selected = (a >= 0 && vis_off[i] >= a && vis_off[i] < b);
            unsigned char c = col[i + MAXCOLS];
            int j = i;
            while (j < shown) {
                int s2 = (a >= 0 && vis_off[j] >= a && vis_off[j] < b);
                if (s2 != selected || col[j + MAXCOLS] != c) break;
                j++;
            }
            if (selected) tty_set_color(TC_BLACK, TC_WHITE);
            else          tty_set_color(c, TC_BLACK);
            write(1, &vis[i], j - i);
            i = j;
        }
        tty_reset_color();
    }

    /* status bar */
    tty_move(rows - 1, 0);
    tty_set_color(TC_BLACK, TC_WHITE);
    printf(" %s%s  %d:%d", path, dirty ? " *" : (saved ? " [saved]" : ""),
           cline + 1, cvcol + 1);
    if (msg[0]) printf("  %s", msg);
    printf("  ^S save ^Q quit ^Z undo ^C/^V copy/paste");
    tty_erase_line();
    tty_reset_color();

    /* cursor */
    int scr_row = cline - top_line;
    int scr_col = GUTTER + cvcol;
    if (scr_row < 0) scr_row = 0;
    if (scr_row >= text_rows) scr_row = text_rows - 1;
    if (scr_col >= cols) scr_col = cols - 1;
    tty_move(scr_row, scr_col);
}

/* --- input --------------------------------------------------------------- */

/* Start, extend or drop the selection as the cursor moves. Moving also ends
 * the current undo group: typing, moving away and typing again should be two
 * undo steps, not one. */
static void update_sel(int shift) {
    if (shift) { if (sel_anchor < 0) sel_anchor = cur; }
    else sel_anchor = -1;
    close_op();
}

static int confirm_discard(void) {
    int cols, rows;
    tty_size(&cols, &rows);
    tty_move(rows - 1, 0);
    tty_set_color(TC_WHITE, TC_RED);
    printf(" unsaved changes -- quit anyway? (y/n) ");
    tty_erase_line();
    tty_reset_color();

    for (;;) {
        struct key_event ev;
        tty_read_key(&ev);
        if (ev.code != KEY_CHAR) continue;
        if (ev.ascii == 'y' || ev.ascii == 'Y') return 1;
        if (ev.ascii == 'n' || ev.ascii == 'N') return 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: edit FILE\n");
        return 1;
    }

    text = (char *)malloc(EDIT_MAX);
    if (!text) {
        printf("edit: out of memory\n");
        return 1;
    }

    char abs[UPATH_MAX];
    upath_resolve("/", argv[1], abs);
    load(abs);
    draw();

    for (;;) {
        struct key_event ev;
        tty_read_key(&ev);
        int shift = (ev.mods & KMOD_SHIFT) != 0;
        msg[0] = '\0';

        if (ev.code == KEY_CHAR && (ev.mods & KMOD_CTRL)) {
            char k = ev.ascii;
            if (k >= 'A' && k <= 'Z') k = k - 'A' + 'a';   /* ctrl+shift+key */

            if (k == 'q') {
                if (dirty && !confirm_discard()) { draw(); continue; }
                break;
            }
            if (k == 's') { save_file(); draw(); continue; }
            if (k == 'c') { copy_selection(0); draw(); continue; }
            if (k == 'x') { copy_selection(1); draw(); continue; }
            if (k == 'v') { paste(); draw(); continue; }
            if (k == 'z') { if (shift) redo(); else undo(); draw(); continue; }
            if (k == 'y') { redo(); draw(); continue; }
            if (k == 'a') { sel_anchor = 0; cur = len; draw(); continue; }
            continue;
        }

        switch (ev.code) {
            case KEY_RESIZE:
                break;

            case KEY_LEFT:
                update_sel(shift);
                if (cur > 0) cur--;
                break;

            case KEY_RIGHT:
                update_sel(shift);
                if (cur < len) cur++;
                break;

            case KEY_UP: {
                update_sel(shift);
                int line = line_of(cur);
                if (line > 0) cur = off_at_vcol(line - 1, vcol_of(cur));
                break;
            }

            case KEY_DOWN: {
                update_sel(shift);
                int line = line_of(cur);
                if (line + 1 < total_lines()) cur = off_at_vcol(line + 1, vcol_of(cur));
                break;
            }

            case KEY_BKSP:
                if (has_sel()) delete_selection();
                else if (cur > 0) { do_delete(cur - 1, 1, 1); cur--; }
                break;

            case KEY_ENTER: {
                delete_selection();
                /* Keep the current line's leading whitespace: without this,
                 * writing indented code means retyping the indent every line. */
                int ls = line_start(line_of(cur));
                char ind[MAXCOLS];
                int n = 0;
                while (ls + n < len && n < MAXCOLS - 2 &&
                       (text[ls + n] == ' ' || text[ls + n] == '\t') &&
                       ls + n < cur) {
                    ind[n + 1] = text[ls + n];
                    n++;
                }
                ind[0] = '\n';
                do_insert(cur, ind, n + 1, 0);
                cur += n + 1;
                break;
            }

            case KEY_CHAR: {
                if (ev.mods & (KMOD_SUPER | KMOD_ALT)) break;
                if (ev.ascii == '\t') {
                    delete_selection();
                    /* Spaces, not a tab: see the note at the top. */
                    int v = vcol_of(cur);
                    int stop = TAB_WIDTH - (v % TAB_WIDTH);
                    char sp[TAB_WIDTH];
                    for (int i = 0; i < stop; i++) sp[i] = ' ';
                    do_insert(cur, sp, stop, 1);
                    cur += stop;
                    break;
                }
                if (ev.ascii >= 32 && ev.ascii < 127) {
                    delete_selection();
                    do_insert(cur, &ev.ascii, 1, 1);
                    cur++;
                }
                break;
            }

            default:
                break;
        }
        draw();
    }

    tty_reset_color();
    tty_clear();
    free(text);
    return 0;
}
