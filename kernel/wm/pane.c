#include "pane.h"
#include "../gfx/font.h"   /* utf8_decode */

#define DEFAULT_ATTR (((PC_DEFAULT_FG) << 4) | (PC_DEFAULT_BG))

static void clamp_dims(uint32_t *cols, uint32_t *rows) {
    if (*cols > PANE_MAX_COLS) *cols = PANE_MAX_COLS;
    if (*rows > PANE_MAX_ROWS) *rows = PANE_MAX_ROWS;
    if (*cols < 1) *cols = 1;
    if (*rows < 1) *rows = 1;
}

static uint8_t cur_attr(struct pane *p) {
    return (uint8_t)((p->cur_fg << 4) | (p->cur_bg & 0x0F));
}

void pane_clear(struct pane *p) {
    for (uint32_t r = 0; r < PANE_MAX_ROWS; r++) {
        for (uint32_t c = 0; c < PANE_MAX_COLS; c++) {
            p->cells[r][c] = ' ';
            p->attr[r][c] = DEFAULT_ATTR;
        }
    }
    p->cursor_col = 0;
    p->cursor_row = 0;
    p->cur_fg = PC_DEFAULT_FG;
    p->cur_bg = PC_DEFAULT_BG;
    p->scroll = 0;   // `clear` also returns the viewport to the live bottom
}

void pane_init(struct pane *p, uint32_t cols, uint32_t rows) {
    clamp_dims(&cols, &rows);
    p->cols = cols;
    p->rows = rows;
    p->in_len = 0;
    p->in_pos = 0;
    p->alive = 1;
    p->hist_head = 0;
    p->hist_count = 0;
    p->scroll = 0;
    p->mq_head = p->mq_tail = 0;
    pane_clear(p);
}

void pane_resize(struct pane *p, uint32_t cols, uint32_t rows) {
    clamp_dims(&cols, &rows);
    p->cols = cols;
    p->rows = rows;
    if (p->cursor_col >= cols) p->cursor_col = cols - 1;
    if (p->cursor_row >= rows) p->cursor_row = rows - 1;
}

void pane_set_color(struct pane *p, uint8_t fg, uint8_t bg) {
    p->cur_fg = fg & 0x0F;
    p->cur_bg = bg & 0x0F;
}

void pane_reset_color(struct pane *p) {
    p->cur_fg = PC_DEFAULT_FG;
    p->cur_bg = PC_DEFAULT_BG;
}

void pane_move(struct pane *p, uint32_t row, uint32_t col) {
    if (row >= p->rows) row = p->rows - 1;
    if (col >= p->cols) col = p->cols - 1;
    p->cursor_row = row;
    p->cursor_col = col;
}

void pane_erase_line(struct pane *p) {
    for (uint32_t c = p->cursor_col; c < p->cols; c++) {
        p->cells[p->cursor_row][c] = ' ';
        p->attr[p->cursor_row][c] = cur_attr(p);
    }
}

// Save row 0 (about to be lost) into the scrollback ring.
static void push_history(struct pane *p) {
    struct hist_line *h = &p->hist[p->hist_head];
    for (uint32_t c = 0; c < PANE_MAX_COLS; c++) {
        h->cells[c] = (c < p->cols) ? p->cells[0][c] : ' ';
        h->attr[c]  = (c < p->cols) ? p->attr[0][c]  : DEFAULT_ATTR;
    }
    p->hist_head = (p->hist_head + 1) % PANE_SCROLLBACK;
    if (p->hist_count < PANE_SCROLLBACK) p->hist_count++;
}

static void scroll_up(struct pane *p) {
    push_history(p);
    for (uint32_t r = 1; r < p->rows; r++) {
        for (uint32_t c = 0; c < p->cols; c++) {
            p->cells[r - 1][c] = p->cells[r][c];
            p->attr[r - 1][c] = p->attr[r][c];
        }
    }
    for (uint32_t c = 0; c < p->cols; c++) {
        p->cells[p->rows - 1][c] = ' ';
        p->attr[p->rows - 1][c] = DEFAULT_ATTR;
    }
    // If the user is reviewing history, grow the offset so what they are
    // looking at stays put rather than sliding as new lines arrive.
    if (p->scroll > 0 && p->scroll < p->hist_count) p->scroll++;
}

void pane_cell(struct pane *p, uint32_t row, uint32_t col, int *ch, uint8_t *attr) {
    // Virtual line index of screen row 0, then of this row. Content is
    // conceptually [history 0..hist_count) followed by [live grid 0..rows).
    int32_t vtop = (int32_t)p->hist_count - (int32_t)p->scroll;
    int32_t v = vtop + (int32_t)row;

    if (v >= 0 && v < (int32_t)p->hist_count) {
        uint32_t ri = (p->hist_head + PANE_SCROLLBACK - p->hist_count + (uint32_t)v)
                      % PANE_SCROLLBACK;
        *ch = p->hist[ri].cells[col];
        *attr = p->hist[ri].attr[col];
        return;
    }
    uint32_t lr = (uint32_t)(v - (int32_t)p->hist_count);
    if (v >= (int32_t)p->hist_count && lr < p->rows) {
        *ch = p->cells[lr][col];
        *attr = p->attr[lr][col];
        return;
    }
    *ch = ' ';
    *attr = DEFAULT_ATTR;
}

void pane_scroll(struct pane *p, int delta) {
    int32_t s = (int32_t)p->scroll + delta;
    if (s < 0) s = 0;
    if (s > (int32_t)p->hist_count) s = (int32_t)p->hist_count;
    p->scroll = (uint32_t)s;
}

void pane_scroll_reset(struct pane *p) {
    p->scroll = 0;
}

static void newline(struct pane *p) {
    p->cursor_col = 0;
    if (p->cursor_row + 1 >= p->rows) {
        scroll_up(p);
    } else {
        p->cursor_row++;
    }
}

/* One character into the grid, once its bytes have all arrived. */
static void pane_put_cp(struct pane *p, int cp) {
    p->cells[p->cursor_row][p->cursor_col] = (uint16_t)cp;
    p->attr[p->cursor_row][p->cursor_col] = cur_attr(p);
    p->cursor_col++;
    if (p->cursor_col >= p->cols) newline(p);
}

void pane_putc(struct pane *p, char c) {
    unsigned char b = (unsigned char)c;

    /* Continuation bytes finish whatever is in hand. A byte that cannot
     * belong to the sequence being built abandons it rather than swallowing
     * the character that follows. */
    if (p->u8n) {
        if ((b & 0xC0) == 0x80) {
            if (p->u8n < 4) p->u8[p->u8n++] = b;
            int need = (p->u8[0] & 0xE0) == 0xC0 ? 2 :
                       (p->u8[0] & 0xF0) == 0xE0 ? 3 : 4;
            if (p->u8n >= need) {
                int cp;
                utf8_decode((const char *)p->u8, p->u8n, &cp);
                p->u8n = 0;
                if (cp > 0xFFFF) cp = '?';
                pane_put_cp(p, cp);
            }
            return;
        }
        p->u8n = 0;                       /* malformed; drop what we had */
    }
    if (b >= 0x80) {
        if ((b & 0xC0) == 0x80) return;   /* a stray continuation byte */
        p->u8[0] = b;
        p->u8n = 1;
        return;
    }

    if (c == '\n') {
        newline(p);
        return;
    }
    if (c == '\r') {
        p->cursor_col = 0;
        return;
    }
    if (c == '\b') {
        if (p->cursor_col > 0) {
            p->cursor_col--;
            p->cells[p->cursor_row][p->cursor_col] = ' ';
            p->attr[p->cursor_row][p->cursor_col] = cur_attr(p);
        }
        return;
    }
    if (c == '\t') {
        for (int i = 0; i < 4; i++) pane_putc(p, ' ');
        return;
    }

    pane_put_cp(p, (unsigned char)c);
}

void pane_write(struct pane *p, const char *s) {
    while (*s) {
        pane_putc(p, *s++);
    }
}
