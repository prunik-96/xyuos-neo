#include "keyboard.h"
#include "ps2mouse.h"
#include "../kernel/process.h"
#include "../wm/wm.h"
#include "../include/port_io.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/pic.h"
#include "../arch/x86_64/pit.h"
#include <stddef.h>

#define KBD_DATA_PORT 0x60

#define BUF_SIZE 256
// ASCII ring (standard shell path)
static volatile char buffer[BUF_SIZE];
static volatile uint32_t buf_head = 0;
static volatile uint32_t buf_tail = 0;

// Event ring (window-manager path)
static volatile struct kbd_event ev_buffer[BUF_SIZE];
static volatile uint32_t ev_head = 0;
static volatile uint32_t ev_tail = 0;

// --- auto-repeat -----------------------------------------------------------
// Hold a key and, after a pause, it starts repeating -- the behaviour every
// desktop has and this one did not.
//
// It has to be done in software because neither input path provides it. USB
// HID has no typematic at all: a held key is simply present in every report
// and never re-sent. The PS/2 controller does repeat, but it cannot be turned
// off (only slowed), and its repeats are filtered out below so the timing
// comes from one place and both keyboards feel identical.
//
// Windows waits about half a second and then runs at roughly thirty a second.
#define REPEAT_DELAY_MS 500
#define REPEAT_RATE_MS   33

static int rep_delay_ms = REPEAT_DELAY_MS;
static int rep_rate_ms  = REPEAT_RATE_MS;

void keyboard_repeat_config(int delay_ms, int rate_ms) {
    if (delay_ms < 150)  delay_ms = 150;    // below this a pause is not felt
    if (delay_ms > 2000) delay_ms = 2000;
    if (rate_ms < 15)    rate_ms = 15;      // ~66/s, past the point of use
    if (rate_ms > 500)   rate_ms = 500;
    rep_delay_ms = delay_ms;
    rep_rate_ms  = rate_ms;
}

int keyboard_repeat_delay(void) { return rep_delay_ms; }
int keyboard_repeat_rate(void)  { return rep_rate_ms; }

static uint8_t  rep_code, rep_mods, rep_active;
static char     rep_ascii;
static uint64_t rep_next_ms;

// PS/2 make codes arrive again and again while a key is held. We now know what
// is down, so those hardware repeats are dropped and replaced by our own.
// Extended codes share the low code space with plain ones (0x1D is left ctrl
// or right ctrl depending on the E0 prefix), hence 256 entries rather than 128.
static uint8_t ps2_held[256];

static int shift_down = 0;
static int ctrl_down = 0;
static int alt_down = 0;
static int super_down = 0;
static int rctrl_down = 0; // Right Ctrl, used as a fallback "mod" alias for
                           // Super in QEMU/on hosts that swallow the Win key
static int extended = 0; // saw the 0xE0 prefix byte

// Scancode Set 1, US QWERTY. Indexed by (scancode & 0x7F); designated
// initializers so this can't silently drift out of position like a plain
// comma list would.
static const char scancode_ascii[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

static const char scancode_ascii_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' ',
};

uint8_t keyboard_mods(void);      // see keyboard.h

static uint8_t current_mods(void) {
    uint8_t m = 0;
    if (shift_down)              m |= KBD_MOD_SHIFT;
    if (ctrl_down)               m |= KBD_MOD_CTRL;
    if (alt_down)                m |= KBD_MOD_ALT;
    // Right Ctrl doubles as the WM "mod" so it works even when the host
    // grabs the Super/Win key (e.g. QEMU on Windows).
    if (super_down || rctrl_down) m |= KBD_MOD_SUPER;
    return m;
}

uint8_t keyboard_mods(void) { return current_mods(); }

static void buf_push(char c) {
    uint32_t next = (buf_head + 1) % BUF_SIZE;
    if (next == buf_tail) return; // full, drop
    buffer[buf_head] = c;
    buf_head = next;
    process_wake_key();
}

static void ev_push_ex(uint8_t code, char ascii, uint8_t pressed);

// A key that has no business repeating: the modifiers (holding Shift must not
// emit a stream of Shifts) and the synthetic ones.
static int repeatable(uint8_t code) {
    return code != KEY_SHIFT && code != KEY_CTRL && code != KEY_ALT &&
           code != KEY_SUPER && code != KEY_RESIZE && code != KEY_FOCUSOUT;
}

static void repeat_arm(uint8_t code, char ascii, uint8_t mods) {
    if (!repeatable(code)) { rep_active = 0; return; }
    rep_code = code;
    rep_ascii = ascii;
    rep_mods = mods;
    rep_active = 1;
    rep_next_ms = pit_get_ticks() * 10 + rep_delay_ms;
}

static void repeat_cancel(uint8_t code) {
    // Only the key that is actually repeating stops it. Letting go of Shift
    // while holding A should not stop the A's.
    if (rep_active && rep_code == code) rep_active = 0;
}

static void ev_push_ex(uint8_t code, char ascii, uint8_t pressed) {
    uint32_t next = (ev_head + 1) % BUF_SIZE;
    if (next == ev_tail) return; // full, drop
    ev_buffer[ev_head].code = code;
    ev_buffer[ev_head].ascii = ascii;
    ev_buffer[ev_head].mods = current_mods();
    ev_buffer[ev_head].pressed = pressed;
    ev_head = next;
    if (pressed) repeat_arm(code, ascii, current_mods());
    else         repeat_cancel(code);
    // Hand the event to the WM immediately, from IRQ context: it either
    // records a key binding for the idle path to act on, or drops the event
    // into the focused pane's queue and wakes its owner. Doing the routing
    // here (rather than in the idle loop) is what avoids a livelock -- waking
    // a process means the idle loop will not run again until it blocks.
    wm_route_input();
}

// Inject a key from a non-PS/2 source (the USB HID driver). Same effect as the
// PS/2 path but with EXPLICIT modifiers, since those come from the HID report
// rather than our tracked shift/ctrl state.
void keyboard_inject_ex(uint8_t code, char ascii, uint8_t mods, uint8_t pressed) {
    if (ascii && pressed) buf_push(ascii);   // typing only; also wakes readers
    uint32_t next = (ev_head + 1) % BUF_SIZE;
    if (next != ev_tail) {
        ev_buffer[ev_head].code = code;
        ev_buffer[ev_head].ascii = ascii;
        ev_buffer[ev_head].mods = mods;
        ev_buffer[ev_head].pressed = pressed;
        ev_head = next;
    }
    if (pressed) repeat_arm(code, ascii, mods);
    else         repeat_cancel(code);
    wm_route_input();
}

void keyboard_inject(uint8_t code, char ascii, uint8_t mods) {
    keyboard_inject_ex(code, ascii, mods, 1);
}

static void keyboard_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    // Bit 5 of the status register says the byte waiting on the shared data
    // port came from the mouse, not the keyboard. Hand it over instead of
    // decoding it as a scancode.
    uint8_t st = inb(0x64);
    if (st & 0x20) { ps2mouse_byte(inb(KBD_DATA_PORT)); return; }
    uint8_t sc = inb(KBD_DATA_PORT);

    if (sc == 0xE0) {
        extended = 1;
        return;
    }

    int released = sc & 0x80;
    uint8_t code = sc & 0x7F;

    // The PS/2 controller repeats a held key by re-sending its make code. We
    // track what is down, so a second make for the same key is that hardware
    // repeat -- dropped here, because auto-repeat is generated centrally with
    // one delay and one rate for every keyboard.
    {
        unsigned slot = (extended ? 128u : 0u) + code;
        if (released) {
            ps2_held[slot] = 0;
        } else {
            if (ps2_held[slot]) { extended = 0; return; }
            ps2_held[slot] = 1;
        }
    }

    if (extended) {
        extended = 0;
        switch (code) {
            case 0x1D: rctrl_down = !released;                    // right ctrl -> WM mod
                       ev_push_ex(KEY_CTRL, 0, !released);  return;
            case 0x38: alt_down = !released;                      // right alt
                       ev_push_ex(KEY_ALT, 0, !released);   return;
            case 0x5B: case 0x5C: super_down = !released;         // left/right GUI
                       ev_push_ex(KEY_SUPER, 0, !released); return;
            case 0x48: ev_push_ex(KEY_UP, 0, !released);    return; // arrow up
            case 0x50: ev_push_ex(KEY_DOWN, 0, !released);  return; // arrow down
            case 0x4B: ev_push_ex(KEY_LEFT, 0, !released);  return; // arrow left
            case 0x4D: ev_push_ex(KEY_RIGHT, 0, !released); return; // arrow right
            case 0x49: ev_push_ex(KEY_PGUP, 0, !released);  return; // page up
            case 0x51: ev_push_ex(KEY_PGDN, 0, !released);  return; // page down
            case 0x53: ev_push_ex(KEY_DEL, 0, !released);   return; // delete
            case 0x47: ev_push_ex(KEY_HOME, 0, !released);  return; // home
            case 0x4F: ev_push_ex(KEY_END, 0, !released);   return; // end
            default: return;
        }
    }

    switch (code) {
        case 0x01: ev_push_ex(KEY_ESC, 0, !released); return;    // escape
        // The modifier bit is updated FIRST, so the event that follows carries
        // the state the key just established rather than the one before it.
        case 0x2A: case 0x36: shift_down = !released;
                   ev_push_ex(KEY_SHIFT, 0, !released); return;
        case 0x1D: ctrl_down = !released;
                   ev_push_ex(KEY_CTRL, 0, !released);  return;
        case 0x38: alt_down = !released;
                   ev_push_ex(KEY_ALT, 0, !released);   return;
    }

    // Function keys are plain (non-extended) scancodes: F1..F10 = 0x3B..0x44,
    // with F11/F12 tacked on at 0x57/0x58.
    if (code >= 0x3B && code <= 0x44) {
        ev_push_ex((uint8_t)(KEY_F1 + (code - 0x3B)), 0, !released);
        return;
    }
    if (code == 0x57) { ev_push_ex(KEY_F11, 0, !released); return; }
    if (code == 0x58) { ev_push_ex(KEY_F12, 0, !released); return; }

    char c = shift_down ? scancode_ascii_shift[code] : scancode_ascii[code];
    if (c == 0) return;

    // The typed-character ring is for TYPING, so only a key going down feeds
    // it. The event ring gets both edges; whoever reads it decides which
    // edges it cares about.
    if (!released) buf_push(c);

    if (c == '\n') {
        ev_push_ex(KEY_ENTER, '\n', !released);
    } else if (c == '\b') {
        ev_push_ex(KEY_BKSP, '\b', !released);
    } else {
        ev_push_ex(KEY_CHAR, c, !released);
    }
}

// Called from the WM's idle tick. Everything it touches is also touched from
// the keyboard IRQ, so the push runs with interrupts off -- that is the
// context ev_push_ex and wm_route_input were written for, and re-entering
// either of them from an IRQ half way through would corrupt the ring.
void keyboard_repeat_tick(void) {
    if (!rep_active) return;
    uint64_t now = pit_get_ticks() * 10;
    if (now < rep_next_ms) return;
    rep_next_ms = now + rep_rate_ms;

    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    if (rep_active) {                       // still held after the barrier
        uint8_t code = rep_code;
        char    ascii = rep_ascii;
        if (ascii) buf_push(ascii);
        uint32_t next = (ev_head + 1) % BUF_SIZE;
        if (next != ev_tail) {
            ev_buffer[ev_head].code = code;
            ev_buffer[ev_head].ascii = ascii;
            ev_buffer[ev_head].mods = rep_mods;
            ev_buffer[ev_head].pressed = 1;
            ev_head = next;
        }
        wm_route_input();
    }
    if (flags & 0x200) __asm__ volatile ("sti" ::: "memory");
}

void keyboard_init(void) {
    irq_register_handler(1, keyboard_irq_handler);
    irq_unmask(1);
}

char keyboard_getchar(void) {
    while (buf_tail == buf_head) {
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
    char c = buffer[buf_tail];
    buf_tail = (buf_tail + 1) % BUF_SIZE;
    return c;
}

int keyboard_poll(char *out) {
    if (buf_tail == buf_head) return 0;
    *out = buffer[buf_tail];
    buf_tail = (buf_tail + 1) % BUF_SIZE;
    return 1;
}

void keyboard_get_event(struct kbd_event *ev) {
    while (ev_tail == ev_head) {
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
    ev->code  = ev_buffer[ev_tail].code;
    ev->ascii = ev_buffer[ev_tail].ascii;
    ev->mods  = ev_buffer[ev_tail].mods;
    ev_tail = (ev_tail + 1) % BUF_SIZE;
}

int keyboard_poll_event(struct kbd_event *ev) {
    if (ev_tail == ev_head) return 0;
    // Copy the whole event, not a list of fields. The list version silently
    // dropped .pressed the moment it was added, and an uninitialised flag on
    // the caller's stack reads as "this key came up" -- so every keystroke in
    // the system was thrown away by the filter that reads it.
    *ev = ev_buffer[ev_tail];
    ev_tail = (ev_tail + 1) % BUF_SIZE;
    return 1;
}

void keyboard_flush(void) {
    buf_tail = buf_head;
    ev_tail = ev_head;
}
