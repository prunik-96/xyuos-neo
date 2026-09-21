// plasma -- a tiny animated graphics demo that exercises the raw pixel surface
// (SYS_GFX). Renders a moving colour field at 320x200; the compositor scales it
// to fill the pane. Press any key to quit.
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#define W 320
#define H 200

int main(void) {
    unsigned int *buf = malloc((unsigned)(W * H) * sizeof(unsigned int));
    if (!buf) { printf("plasma: out of memory\n"); return 1; }

    int quit = 0;
    while (!quit) {
        // Any key quits -- but a synthetic event is not a key.
        //
        // The compositor delivers XKEY_RESIZE to a pane whose geometry
        // changed, and opening a window changes the geometry of the window
        // being opened. Treating every event alike meant this program quit
        // during its own start-up whenever it was launched into a new window,
        // which is exactly what the start menu does. From a shell, where the
        // pane already existed, it ran fine -- so it looked like the start
        // menu was broken rather than the event loop.
        key_event_t ev;
        while (poll_event(&ev)) {
            if (ev.code == XKEY_RESIZE) continue;   // redraw, do not exit
            quit = 1;
        }
        if (quit) break;

        unsigned int t = uptime_ms() / 16;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                unsigned char r = (unsigned char)(x + t);
                unsigned char g = (unsigned char)(y + t * 2);
                unsigned char b = (unsigned char)((x ^ y) + t);
                buf[y * W + x] = ((unsigned int)r << 16) |
                                 ((unsigned int)g << 8)  | b;
            }
        }
        gfx_blit(buf, W, H);
        sleep_ms(16);
    }

    gfx_end();
    free(buf);
    return 0;
}
