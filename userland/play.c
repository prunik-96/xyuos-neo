/* play -- the music player.
 *
 * WAV, because it is the format a system with its own audio stack should be
 * able to play before it can play any other: everything else (MP3, Vorbis,
 * FLAC) is a decoder bolted onto this same pipe. What the file gives you is
 * some number of channels at some rate in some sample width; what the
 * hardware takes is exactly 48 kHz, 16-bit, stereo. Everything interesting
 * here is that conversion, done a block at a time so a forty-megabyte file
 * does not have to be resampled into memory before the first note.
 *
 * The resampler is nearest-neighbour on purpose. Linear interpolation is four
 * lines more and audibly better on a big rate change, but 44.1 -> 48 is a
 * ratio of 1.088: the error is a fraction of a sample and it costs a
 * multiply per output frame in a loop that has to keep ahead of the DMA. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "gui.h"
#include "upath.h"

#define OUT_RATE  48000
#define CHUNK     4096            /* output frames per top-up */

static char path[UPATH_MAX];
static char status[200];

/* --- the WAV header ------------------------------------------------------- */

static long   fd = -1;
static int    wav_ch, wav_bits;
static long   wav_rate;
static long   data_start, data_bytes, data_pos;
static long   total_frames, played_frames;

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int wav_open(const char *p) {
    if (fd >= 0) { xyuos_close(fd); fd = -1; }
    fd = xyuos_open(p);
    if (fd < 0) { snprintf(status, sizeof status, "cannot open %s", p); return 0; }

    unsigned char hdr[12];
    if (xyuos_read(fd, hdr, 12) != 12 ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        snprintf(status, sizeof status, "not a WAV file");
        xyuos_close(fd); fd = -1;
        return 0;
    }

    /* Walk the chunk list. `fmt ` describes the samples, `data` holds them,
     * and everything in between (LIST, fact, whatever a tagger left behind)
     * is skipped by its own length field. */
    long off = 12;
    int have_fmt = 0;
    for (;;) {
        unsigned char ch[8];
        if (xyuos_seek(fd, off, SEEK_SET) < 0) break;
        if (xyuos_read(fd, ch, 8) != 8) break;
        unsigned long len = rd32(ch + 4);

        if (!memcmp(ch, "fmt ", 4) && len >= 16) {
            unsigned char f[16];
            if (xyuos_read(fd, f, 16) != 16) break;
            unsigned fmt = rd16(f);
            wav_ch   = (int)rd16(f + 2);
            wav_rate = (long)rd32(f + 4);
            wav_bits = (int)rd16(f + 14);
            if (fmt != 1 && fmt != 0xFFFE) {
                snprintf(status, sizeof status, "compressed WAV (format %u)", fmt);
                xyuos_close(fd); fd = -1;
                return 0;
            }
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            data_start = off + 8;
            data_bytes = (long)len;
            break;
        }
        off += 8 + (long)len + ((long)len & 1);   /* chunks are word-aligned */
    }

    if (!have_fmt || !data_bytes) {
        snprintf(status, sizeof status, "WAV has no audio data");
        xyuos_close(fd); fd = -1;
        return 0;
    }
    if (wav_bits != 8 && wav_bits != 16 && wav_bits != 24 && wav_bits != 32) {
        snprintf(status, sizeof status, "unsupported sample width (%d bit)", wav_bits);
        xyuos_close(fd); fd = -1;
        return 0;
    }
    if (wav_ch < 1 || wav_ch > 8) {
        snprintf(status, sizeof status, "unsupported channel count (%d)", wav_ch);
        xyuos_close(fd); fd = -1;
        return 0;
    }

    int frame_bytes = wav_ch * wav_bits / 8;
    total_frames = data_bytes / frame_bytes;
    data_pos = 0;
    played_frames = 0;
    xyuos_seek(fd, data_start, SEEK_SET);
    snprintf(path, sizeof path, "%s", p);
    snprintf(status, sizeof status, "%ld Hz, %d ch, %d bit", wav_rate, wav_ch, wav_bits);
    return 1;
}

/* --- decode + convert ------------------------------------------------------ */

static unsigned char raw[CHUNK * 8 * 4];   /* worst case: 8ch x 32-bit */
static short         out[CHUNK * 2];

/* One sample, whatever width it was stored at, as signed 16-bit. */
static int sample_at(const unsigned char *p, int bits) {
    switch (bits) {
        case 8:  return ((int)p[0] - 128) << 8;        /* 8-bit WAV is unsigned */
        case 16: return (short)(p[0] | (p[1] << 8));
        case 24: return (int)((signed char)p[2]) * 256 + p[1];
        default: return (int)((signed char)p[3]) * 256 + p[2];   /* 32-bit */
    }
}

/* Fill `out` with up to `want` output frames. Returns how many were produced;
 * 0 means the file is finished. */
static int decode(int want) {
    if (fd < 0 || data_pos >= data_bytes) return 0;

    int frame_bytes = wav_ch * wav_bits / 8;
    /* How many INPUT frames this many output frames needs. */
    long need_in = ((long)want * wav_rate + OUT_RATE - 1) / OUT_RATE + 1;
    long avail = (data_bytes - data_pos) / frame_bytes;
    if (need_in > avail) need_in = avail;
    if (need_in <= 0) return 0;
    if (need_in > (long)(sizeof raw) / frame_bytes) need_in = (long)(sizeof raw) / frame_bytes;

    long got = xyuos_read(fd, raw, (unsigned long)(need_in * frame_bytes));
    if (got <= 0) return 0;
    long in_frames = got / frame_bytes;
    data_pos += in_frames * frame_bytes;

    int produced = 0;
    for (int i = 0; i < want; i++) {
        long src = (long)i * wav_rate / OUT_RATE;
        if (src >= in_frames) break;
        const unsigned char *f = raw + src * frame_bytes;
        int l = sample_at(f, wav_bits);
        int r = (wav_ch > 1) ? sample_at(f + wav_bits / 8, wav_bits) : l;
        out[produced * 2 + 0] = (short)l;
        out[produced * 2 + 1] = (short)r;
        produced++;
    }
    played_frames += (long)produced * wav_rate / OUT_RATE;
    return produced;
}

/* --- the window ------------------------------------------------------------ */

#define TOOL_H 44
#define STAT_H 26

static int playing, vol = 80;
static int bars[48];                /* a cheap level meter */

static const char *btn_label[] = { "Play", "Pause", "Stop", "Vol -", "Vol +" };
#define NBTN ((int)(sizeof btn_label / sizeof btn_label[0]))

static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int bw = g->fw * 8, gap = 6;
    *w = bw; *h = TOOL_H - 12; *x = 10 + i * (bw + gap); *y = 6;
}

static void draw(gui_t *g) {
    gui_clear(g, GC_WIN);

    gui_vgrad(g, 0, 0, g->w, TOOL_H, GC_PANEL, GC_BAR);
    gui_fill(g, 0, TOOL_H - 1, g->w, 1, GC_EDGE);
    for (int i = 0; i < NBTN; i++) {
        int x, y, w, h;
        btn_rect(g, i, &x, &y, &w, &h);
        int st = gui_in(g->mx, g->my, x, y, w, h) ? GB_HOVER : GB_NORMAL;
        if (i == 0 && playing) st = GB_OFF;
        if (i == 1 && !playing) st = GB_OFF;
        gui_button(g, x, y, w, h, btn_label[i], st);
    }
    {
        char t[32];
        snprintf(t, sizeof t, "volume %d%%", vol);
        int x = 10 + NBTN * (g->fw * 8 + 6) + 10;
        gui_text(g, x, (TOOL_H - g->fh) / 2, t, GC_DIM);
    }

    /* level meter -- something has to move while a sound file plays */
    int my0 = TOOL_H + 20, mh = g->h - TOOL_H - STAT_H - 116;
    if (mh > 30) {
        gui_panel(g, 16, my0, g->w - 32, mh, GC_PANEL, GC_EDGE);
        int n = (int)(sizeof bars / sizeof bars[0]);
        int bw = (g->w - 44) / n;
        for (int i = 0; i < n; i++) {
            int hgt = bars[i] * (mh - 12) / 32768;
            if (hgt < 1) hgt = 1;
            unsigned c = hgt > (mh - 12) * 3 / 4 ? 0xE05A3A :
                         hgt > (mh - 12) / 2     ? 0xE0C040 : 0x46C06A;
            gui_fill(g, 22 + i * bw, my0 + mh - 6 - hgt, bw - 2, hgt, c);
        }
    }

    /* progress */
    int py = g->h - STAT_H - 54;
    gui_text_clip(g, 16, py - g->fh - 8, path, GC_TEXT, g->w - 32);
    gui_panel(g, 16, py, g->w - 32, 16, GC_PANEL, GC_EDGE);
    if (total_frames > 0) {
        long done = played_frames;
        if (done > total_frames) done = total_frames;
        int fillw = (int)((long)(g->w - 34) * done / total_frames);
        gui_fill(g, 17, py + 1, fillw, 14, GC_ACCENT);
    }
    {
        char t[80];
        long secs = wav_rate ? played_frames / wav_rate : 0;
        long tot  = wav_rate ? total_frames / wav_rate : 0;
        snprintf(t, sizeof t, "%ld:%02ld / %ld:%02ld",
                 secs / 60, secs % 60, tot / 60, tot % 60);
        gui_text(g, 16, py + 24, t, GC_DIM);
        gui_text(g, g->w - 16 - gui_tw(g, playing ? "playing" : "stopped", 1),
                 py + 24, playing ? "playing" : "stopped",
                 playing ? GC_GOOD : GC_DIM);
    }

    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    gui_text_clip(g, 10, g->h - STAT_H + (STAT_H - g->fh) / 2, status,
                  GC_TEXT, g->w - 20);
}

static void restart(void) {
    if (fd < 0) return;
    xyuos_seek(fd, data_start, SEEK_SET);
    data_pos = 0;
    played_frames = 0;
}

static void do_button(int i) {
    switch (i) {
        case 0:
            if (fd < 0) { snprintf(status, sizeof status, "nothing loaded"); break; }
            if (data_pos >= data_bytes) restart();
            playing = 1;
            break;
        case 1: playing = 0; audio_flush(); break;
        case 2: playing = 0; audio_flush(); restart(); break;
        case 3: vol -= 10; if (vol < 0) vol = 0; audio_volume(vol); break;
        case 4: vol += 10; if (vol > 100) vol = 100; audio_volume(vol); break;
        default: break;
    }
}

int main(int argc, char **argv) {
    gui_t g;
    if (!gui_open(&g)) return 1;

    if (!audio_present()) {
        message_box(MSG_WARN, "AUDIO-NODEV", "No sound device",
                    "This machine has no HD Audio codec the system can drive.",
                    "System sounds will use the PC speaker instead.");
        snprintf(status, sizeof status, "no audio device -- nothing will be heard");
    }
    audio_volume(vol);

    if (argc > 1) {
        char abs[UPATH_MAX];
        upath_resolve("/", argv[1], abs);
        if (wav_open(abs)) playing = 1;
        else message_box(MSG_ERROR, "WAV-OPEN", "Cannot play this file",
                         status, argv[1]);
    } else {
        /* Same idea as the viewer: with nothing named, play the first thing
         * in the sounds folder. */
        static char buf[4096];
        long n = xyuos_listdir("/sounds", buf, sizeof buf);
        char pick[UPATH_MAX];
        pick[0] = 0;
        for (long i = 0; i < n && !pick[0]; ) {
            long j = i;
            while (j < n && buf[j] != '\n') j++;
            int len = (int)(j - i);
            if (len > 0 && buf[j - 1] != '/') {
                char name[96];
                if (len > 95) len = 95;
                memcpy(name, buf + i, (size_t)len);
                name[len] = 0;
                upath_resolve("/sounds", name, pick);
            }
            i = j + 1;
        }
        if (pick[0] && wav_open(pick)) playing = 1;
        else snprintf(status, sizeof status, "usage: play <file.wav>");
    }

    int running = 1, dirty = 1, hover_btn = -2;

    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type == GE_KEY) {
                dirty = 1;
                key_event_t *k = &e.k;
                if (k->code == XKEY_RESIZE) { gui_sync(&g); continue; }
                if (k->code == XKEY_ESC) running = 0;
                else if (k->code == XKEY_CHAR && k->ascii == ' ')
                    do_button(playing ? 1 : 0);
                else if (k->code == XKEY_CHAR && k->ascii == 'q') running = 0;
                else if (k->code == XKEY_UP)   do_button(4);
                else if (k->code == XKEY_DOWN) do_button(3);
                continue;
            }
            mouse_event_t *m = &e.m;
            {
                int hb = -1;
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (gui_in(m->x, m->y, x, y, w, h)) hb = i;
                }
                if (hb != hover_btn) { hover_btn = hb; dirty = 1; }
            }
            if (gui_acts(&e)) dirty = 1;
            if (m->pressed & MB_LEFT) {
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (gui_in(m->x, m->y, x, y, w, h)) do_button(i);
                }
            }
        }

        /* Keep the hardware ring fed. audio_play() takes what it can and
         * returns; whatever it refused is simply offered again next time
         * round, which is the whole of the flow control. */
        if (playing) {
            int pending = audio_pending();
            if (pending < OUT_RATE / 4) {          /* under 250 ms buffered */
                int n = decode(CHUNK);
                if (n <= 0) {
                    playing = 0;
                    snprintf(status, sizeof status, "finished");
                } else {
                    int off = 0;
                    while (off < n) {
                        int took = audio_play(out + off * 2, n - off);
                        if (took <= 0) break;
                        off += took;
                    }
                    /* Feed the meter from what we just sent. */
                    int nb = (int)(sizeof bars / sizeof bars[0]);
                    for (int i = 0; i < nb; i++) {
                        int idx = i * n / nb;
                        int v = out[idx * 2];
                        bars[i] = v < 0 ? -v : v;
                    }
                }
                dirty = 1;
            }
        }

        if (dirty) {
            if (gui_sync(&g)) {
                draw(&g);
                gui_present(&g);
                dirty = 0;
            } else if (gui_lost(&g)) {
                break;          /* the window really is gone */
            }
        }
        sleep_ms(playing ? 20 : 60);
        if (playing) dirty = 1;
    }

    audio_flush();
    if (fd >= 0) xyuos_close(fd);
    gui_close(&g);
    return 0;
}
