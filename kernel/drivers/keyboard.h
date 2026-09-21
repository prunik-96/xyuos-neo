#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

// Modifier bitmask for kbd_event.mods
#define KBD_MOD_SHIFT 0x01
#define KBD_MOD_CTRL  0x02
#define KBD_MOD_ALT   0x04
#define KBD_MOD_SUPER 0x08

// Special (non-ASCII) key codes reported via kbd_event.code. Printable keys
// carry their ASCII in .ascii and KEY_CHAR in .code.
#define KEY_CHAR   0x00
#define KEY_UP     0x01
#define KEY_DOWN   0x02
#define KEY_LEFT   0x03
#define KEY_RIGHT  0x04
#define KEY_ENTER  0x05
#define KEY_BKSP   0x06
// Synthetic: not a key at all. The compositor delivers it to a pane whose
// geometry changed, so a full-screen program knows to redraw itself.
#define KEY_RESIZE 0x07
#define KEY_PGUP   0x08
#define KEY_PGDN   0x09
// Editing keys. GUI programs need these; the text tools never did, which
// is why they only appear now.
#define KEY_DEL    0x0A
#define KEY_HOME   0x0B
#define KEY_END    0x0C
#define KEY_ESC    0x0D
// Function keys, so the window manager can offer the shortcuts people expect
// (Alt+F4 to close a window).
#define KEY_F1     0x10
#define KEY_F2     0x11
#define KEY_F3     0x12
#define KEY_F4     0x13
#define KEY_F5     0x14
#define KEY_F6     0x15
#define KEY_F7     0x16
#define KEY_F8     0x17
#define KEY_F9     0x18
#define KEY_F10    0x19
#define KEY_F11    0x1A
#define KEY_F12    0x1B

// The modifier keys, reported as keys in their own right.
//
// A text program wants Shift as a BIT on the letter it modifies, which is what
// .mods is for. A game wants Shift as a key it can see go down and come back
// up -- in DOOM, Shift is the run key, and Ctrl is fire. Both views are needed,
// so both are provided; these codes only reach a program that asked for them.
#define KEY_SHIFT  0x0E
#define KEY_CTRL   0x0F
#define KEY_ALT    0x1C
#define KEY_SUPER  0x1D

// Synthetic, like KEY_RESIZE: the pane just lost focus, so every key it
// believes is held should be considered released. Without it, Alt+Tabbing out
// of a game leaves it walking forward forever -- it saw the key go down and
// will never see it come up, because the keyboard went somewhere else.
#define KEY_FOCUSOUT 0x1E

struct kbd_event {
    uint8_t code;   // KEY_CHAR for printable keys, else a KEY_* arrow/etc.
    char    ascii;  // valid when code == KEY_CHAR
    uint8_t mods;   // KBD_MOD_* bitmask at time of press
    // 1 = the key went down (or the hardware repeated it), 0 = it came back up.
    //
    // Until now only key-DOWN existed anywhere in this system, which is fine
    // for typing and useless for anything that needs to know what is being
    // HELD: a program could tell that you pressed W, never that you were still
    // pressing it. That is why walking in DOOM meant tapping the key.
    uint8_t pressed;
};

void keyboard_init(void);

// Drive the auto-repeat clock. Called from the WM's idle tick (~100 Hz); a
// key held past the initial delay starts producing repeats from here.
void keyboard_repeat_tick(void);

// Auto-repeat timings, in milliseconds: how long a key must be held before it
// starts repeating, and how long between repeats afterwards.
void keyboard_repeat_config(int delay_ms, int rate_ms);
int  keyboard_repeat_delay(void);
int  keyboard_repeat_rate(void);

// Modifier keys held right now (KBD_MOD_* bitmask). The window manager uses it
// to notice when Alt is let go, which is what ends an Alt+Tab pass.
uint8_t keyboard_mods(void);

// Inject a key event from the USB HID driver (code = KEY_*, ascii, mods =
// KBD_MOD_* bitmask), with an explicit direction. Routes it exactly like a
// PS/2 keypress. keyboard_inject() is the key-down case.
void keyboard_inject_ex(uint8_t code, char ascii, uint8_t mods, uint8_t pressed);

// Inject a key event from the USB HID driver (code = KEY_*, ascii, mods =
// KBD_MOD_* bitmask). Routes it exactly like a PS/2 keypress.
void keyboard_inject(uint8_t code, char ascii, uint8_t mods);

// --- ASCII path (used by the standard fullscreen shell via SYS_READKEY) ---
char keyboard_getchar(void);          // blocks until a char is available
int  keyboard_poll(char *out);        // non-blocking; 1 if a char was dequeued

// --- Event path (used by the in-kernel window manager) ---
void keyboard_get_event(struct kbd_event *ev);  // blocks until an event
int  keyboard_poll_event(struct kbd_event *ev); // non-blocking; 1 if dequeued
void keyboard_flush(void);                       // drop all queued input/events

#endif
