// doomgeneric platform layer for xyuOS Neo.
//
// The whole platform is four things: blit a 32-bit frame to our pane
// (SYS_GFX), a millisecond clock (SYS_UPTIME), a sleep (SYS_SLEEP), and
// keyboard input.
//
// Input used to be a lie. The OS reported only key-DOWN, so this file faked a
// key-UP 220 ms after each press and relied on the keyboard's auto-repeat to
// re-arm it. That is why you could not walk: on a USB keyboard there IS no
// auto-repeat -- a held key is simply present in every report and re-sent
// never -- so one press bought 220 ms of movement and then stopped. The OS
// now reports both edges (key_events_raw), so DOOM gets the real thing and
// this file just tracks state.

#include "doomkeys.h"
#include "doomgeneric.h"

#include <unistd.h>     // gfx_blit, poll_event, uptime_ms, sleep_ms, key_event_t, XKEY_*
#include <stdlib.h>
#include <string.h>

#define KEYQUEUE_SIZE 64
static unsigned short s_queue[KEYQUEUE_SIZE];
static unsigned int   s_qw = 0, s_qr = 0;

// What DOOM currently believes is held, so a repeat does not look like a
// second press and a focus change can let go of everything.
static unsigned char s_down[256];

static void push(int pressed, unsigned char key) {
    s_queue[s_qw] = (unsigned short)((pressed << 8) | key);
    s_qw = (s_qw + 1) % KEYQUEUE_SIZE;
    if (s_qw == s_qr) s_qr = (s_qr + 1) % KEYQUEUE_SIZE;  // drop oldest
}

// Map one of our key events to a DOOM key code. The special action codes
// (KEY_FIRE/KEY_USE) are bound by DOOM regardless of the config, which lets us
// fire/open without a real Ctrl key (we only get Ctrl as a modifier bit).
static unsigned char to_doom(const key_event_t *ev) {
    switch (ev->code) {
        case XKEY_UP:    return KEY_UPARROW;
        case XKEY_DOWN:  return KEY_DOWNARROW;
        case XKEY_LEFT:  return KEY_LEFTARROW;
        case XKEY_RIGHT: return KEY_RIGHTARROW;
        case XKEY_ENTER: return KEY_ENTER;
        case XKEY_BKSP:  return KEY_BACKSPACE;
        // The modifiers, now that they arrive as keys: Shift runs, Ctrl fires.
        case XKEY_SHIFT: return KEY_RSHIFT;
        case XKEY_CTRL:  return KEY_FIRE;
        case XKEY_ALT:   return KEY_RALT;
        // Escape arrives as a CODE with no ascii, so the ascii test below
        // never saw it -- which meant the in-game menu could not be opened at
        // all, and therefore the game could not be quit from inside itself.
        case XKEY_ESC:   return KEY_ESCAPE;
        default: break;
    }
    // Function keys: help, save, load, and F10 to quit. Also code-only, and
    // also invisible to an ascii test. F1..F10 are contiguous in DOOM's table;
    // F11 and F12 are not.
    if (ev->code >= XKEY_F1 && ev->code <= XKEY_F(10))
        return (unsigned char)(KEY_F1 + (ev->code - XKEY_F1));
    if (ev->code == XKEY_F(11)) return KEY_F11;
    if (ev->code == XKEY_F(12)) return KEY_F12;

    unsigned char a = (unsigned char)ev->ascii;
    if (a == 27)   return KEY_ESCAPE;
    if (a == '\t') return KEY_TAB;
    if (a == ' ')  return KEY_FIRE;              // space fires
    if (a == 'e' || a == 'E') return KEY_USE;    // E opens doors / switches
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');  // DOOM wants lowercase
    return a;
}

// Turn real key edges into DOOM's press/release queue.
static void pump_input(void) {
    key_event_t ev;
    while (poll_event(&ev)) {
        if (ev.code == XKEY_FOCUSOUT) {
            // The keyboard went to another window. Let go of everything, or we
            // keep running in whatever direction we were going.
            for (int k = 0; k < 256; k++) {
                if (s_down[k]) { s_down[k] = 0; push(0, (unsigned char)k); }
            }
            continue;
        }
        unsigned char k = to_doom(&ev);
        if (!k) continue;
        // A PS/2 keyboard repeats a held key in hardware. DOOM already knows
        // it is down, so a repeat is noise.
        if (ev.pressed && s_down[k]) continue;
        if (!ev.pressed && !s_down[k]) continue;
        s_down[k] = ev.pressed;
        push(ev.pressed, k);
    }
}

void DG_Init(void) {
    // Ask for the whole keyboard: releases, and the modifier keys themselves.
    // Without this the pane only ever delivers what was typed.
    key_events_raw(1);
}

void DG_DrawFrame(void) {
    pump_input();
    gfx_blit(DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms) { sleep_ms(ms); }

uint32_t DG_GetTicksMs(void) { return uptime_ms(); }

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    if (s_qr == s_qw) return 0;
    unsigned short d = s_queue[s_qr];
    s_qr = (s_qr + 1) % KEYQUEUE_SIZE;
    *pressed = d >> 8;
    *doomKey = d & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    // Build an argv that always has an IWAD. Default to /doom1.wad on our disk.
    static char *av[16];
    int ac = 0, has_iwad = 0;
    av[ac++] = "doom";
    for (int i = 1; i < argc && ac < 12; i++) {
        av[ac++] = argv[i];
        if (!strcmp(argv[i], "-iwad")) has_iwad = 1;
    }
    if (!has_iwad) { av[ac++] = "-iwad"; av[ac++] = "/doom1.wad"; }

    doomgeneric_Create(ac, av);
    for (;;) doomgeneric_Tick();
    return 0;
}
