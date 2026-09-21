#ifndef WM_PANE_H
#define WM_PANE_H

#include <stdint.h>
#include "../drivers/keyboard.h"

// Character grid dimensions cap. A pane never exceeds these; its live
// cols/rows are set to fit its current on-screen rectangle. Sized to cover a
// full 1920x1080 framebuffer at the smallest cell we might use (~9px wide ->
// ~213 cols, ~16px tall -> ~67 rows), plus margin, so a fullscreen pane fills
// the whole screen at Full HD with the anti-aliased font.
#define PANE_MAX_COLS 240
#define PANE_MAX_ROWS 136

// Palette color indices (match the WM renderer palette).
#define PC_BLACK   0
#define PC_RED     1
#define PC_GREEN   2
#define PC_YELLOW  3
#define PC_BLUE    4
#define PC_MAGENTA 5
#define PC_CYAN    6
#define PC_WHITE   7
#define PC_DEFAULT_FG PC_WHITE
#define PC_DEFAULT_BG PC_BLACK

// Longest line the terminal will gather for a stdin read.
#define PANE_INPUT_MAX 256

// Per-pane input queue. The keyboard IRQ fills the global queue; the WM drains
// it and routes each event to the focused pane, where the owning process picks
// it up via SYS_READEVENT. Deep enough to absorb fast typing while a process is
// busy, small enough that 16 panes stay affordable.
#define PANE_EVQ_MAX 64

// Per-pane pointer queue. The compositor routes a pointer event here when it
// lands in the CLIENT area of the focused window, with the coordinates already
// translated into the program's own surface space -- a GUI program should
// never have to know where its window happens to sit on screen.
#define PANE_MOUSEQ_MAX 32

// How long a drag payload may be: one absolute path, comfortably.
#define PANE_DROP_MAX 288

struct pane_mouse {
    int16_t x, y;        // client-relative, in the program's surface pixels
    uint8_t buttons;     // MOUSE_* held right now
    uint8_t pressed;     // went down in this event
    uint8_t released;    // came up in this event
    int8_t  wheel;
    // 0 = ordinary pointer event. 1 = something is being dragged over this
    // window. 2 = it was dropped here, and the payload is waiting. 3 = the
    // drag has moved off this window -- without this a window keeps showing
    // "drop here" long after the pointer has gone somewhere else.
    uint8_t drag;
};

// Lines of scrollback kept per pane. A row that scrolls off the top is saved
// here rather than lost, so the user can page up to review it.
#define PANE_SCROLLBACK 256

struct hist_line {
    uint16_t cells[PANE_MAX_COLS];   // code points, not bytes
    uint8_t attr[PANE_MAX_COLS];
};

struct pane {
    // character grid (row-major); only [0..rows) x [0..cols) is live.
    // Code points rather than bytes: a cell holds one character, and a
    // character is not one byte outside ASCII.
    uint16_t cells[PANE_MAX_ROWS][PANE_MAX_COLS];
    uint8_t  attr[PANE_MAX_ROWS][PANE_MAX_COLS];  // (fg << 4) | bg per cell
    uint32_t cols;
    uint32_t rows;
    uint32_t cursor_col;
    uint32_t cursor_row;
    // Partial UTF-8 sequence: pane_putc is fed bytes and has to wait for
    // the rest of a character before it can store one.
    unsigned char u8[4];
    int      u8n;

    uint8_t  cur_fg;   // colors used by subsequent pane_putc
    uint8_t  cur_bg;

    // --- scrollback ---
    // Ring of lines evicted from the top. `hist_head` is the next write slot;
    // `hist_count` how many lines are valid. `scroll` is how far the viewport
    // is pushed up into history (0 = live bottom).
    struct hist_line hist[PANE_SCROLLBACK];
    uint32_t hist_head;
    uint32_t hist_count;
    uint32_t scroll;

    int      alive;   // slot in use

    // --- ownership ---
    // pid of the process driving this pane. Its input arrives through evq.
    int      owner_pid;
    struct kbd_event evq[PANE_EVQ_MAX];
    uint32_t evq_head, evq_tail;
    struct pane_mouse mq[PANE_MOUSEQ_MAX];
    uint32_t mq_head, mq_tail;

    // What was dropped on this window, waiting to be collected. Kept per pane
    // rather than globally so a drop can only ever be read by the program it
    // was aimed at.
    char     drop[PANE_DROP_MAX];
    int      drop_ready;

    // 1 if this pane belongs to a program launched from the start menu. Such
    // a window closes when its program exits; a terminal pane instead gets a
    // fresh shell, because an empty terminal is still a useful thing and an
    // empty application window is not.
    int      app_pane;
    // Set by the program: deliver key-UP events and the modifier keys
    // themselves, not just key-down. Off by default, and that default is the
    // point -- an editor that suddenly received a release for every press
    // would type everything twice. A program that tracks held keys asks for
    // them; every program written before this existed keeps working untouched.
    int      keys_raw;
    // WHICH process asked. A program started from the shell does not own the
    // pane -- the shell does -- so "the owner exited" never fires for it, and
    // the mode would outlive it. The shell would then receive a release for
    // every press and echo every character twice. Same reasoning as gfx_pid.
    int      keys_raw_pid;

    // --- graphics mode (a program blitting raw pixels, e.g. DOOM) ---
    // When gfx_on, the compositor blits this ARGB buffer (gfx_w x gfx_h),
    // scaled to the pane interior, instead of drawing the character grid. The
    // buffer is kernel-owned (kmalloc'd on first blit, freed on pane close).
    uint32_t *gfx;
    uint16_t  gfx_w, gfx_h;
    int       gfx_on;
    // The process painting this surface. Not always the pane's owner: a
    // program started from the shell is a child, and when it exits the pane
    // has to be handed back to the terminal underneath it.
    int       gfx_pid;

    // --- line discipline for stdin (SYS_READSTD) ---
    // A terminal read is line-at-a-time, like canonical mode: the kernel
    // gathers a whole line, echoing it and handling backspace, and only then
    // hands characters to the program. Without this a program calling
    // scanf() or getchar() would leave the user typing blind, since nothing
    // else echoes keystrokes.
    char     in_line[PANE_INPUT_MAX];
    uint32_t in_len;    // bytes gathered
    uint32_t in_pos;    // bytes already handed out
};

// Reset a pane to an empty grid of the given size.
void pane_init(struct pane *p, uint32_t cols, uint32_t rows);

// Resize the grid, preserving as much content as fits (top-left anchored).
void pane_resize(struct pane *p, uint32_t cols, uint32_t rows);

// Append a character to the pane's grid, honoring \n, \r, \b and scrolling
// within the pane when the bottom row overflows. Uses the current cur_fg/bg.
void pane_putc(struct pane *p, char c);
void pane_write(struct pane *p, const char *s);

// Clear the whole grid and home the cursor (resets colors to default).
void pane_clear(struct pane *p);

// --- direct-addressing primitives for TUI modes (fm / editor) ---
void pane_set_color(struct pane *p, uint8_t fg, uint8_t bg);
void pane_reset_color(struct pane *p);
void pane_move(struct pane *p, uint32_t row, uint32_t col);   // 0-based
void pane_erase_line(struct pane *p);                          // cursor..EOL

// --- scrollback ---
// The character + attribute the RENDERER should draw at screen (row, col),
// accounting for the scroll offset. Reads history when scrolled up.
void pane_cell(struct pane *p, uint32_t row, uint32_t col, int *cp, uint8_t *attr);
// Move the viewport by `delta` lines: positive = up into history, negative =
// back down toward the live bottom. Clamped to the available history.
void pane_scroll(struct pane *p, int delta);
// Jump back to the live bottom (scroll = 0). Called when the user interacts.
void pane_scroll_reset(struct pane *p);

#endif
