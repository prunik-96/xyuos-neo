#ifndef TTY_H
#define TTY_H

/* Userland view of a WM pane.
 *
 * The character grid belongs to the kernel compositor; a process reaches only
 * its own pane, and only through these calls -- there is no pane handle to
 * forge. Ordinary printf()/write() output lands in the same grid.
 *
 * Colors are palette indices shared with the compositor. */

#define TC_BLACK   0
#define TC_RED     1
#define TC_GREEN   2
#define TC_YELLOW  3
#define TC_BLUE    4
#define TC_MAGENTA 5
#define TC_CYAN    6
#define TC_WHITE   7

void tty_clear(void);
void tty_move(int row, int col);        /* 0-based */
void tty_set_color(int fg, int bg);
void tty_reset_color(void);
void tty_erase_line(void);              /* cursor to end of line */
void tty_size(int *cols, int *rows);
void tty_getcur(int *row, int *col);    /* where the cursor is now */

/* Key events, as delivered to the focused pane. Mirrors the kernel's
 * struct kbd_event; the packed form comes from xyuos_readevent(). */
#define KEY_CHAR   0x00
#define KEY_UP     0x01
#define KEY_DOWN   0x02
#define KEY_LEFT   0x03
#define KEY_RIGHT  0x04
#define KEY_ENTER  0x05
#define KEY_BKSP   0x06
/* Not a key: the compositor sends this when the pane's geometry changed, so a
 * full-screen program can redraw itself at the new size. */
#define KEY_RESIZE 0x07

#define KMOD_SHIFT 0x01
#define KMOD_CTRL  0x02
#define KMOD_ALT   0x04
#define KMOD_SUPER 0x08

struct key_event {
    int  code;    /* KEY_* */
    char ascii;   /* valid when code == KEY_CHAR */
    int  mods;    /* KMOD_* bitmask */
};

/* Blocks until a key arrives in this pane. */
void tty_read_key(struct key_event *ev);

#endif
