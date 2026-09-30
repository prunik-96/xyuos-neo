/* play -- the music player.
 *
 * WAV, MP3 and Ogg Vorbis. Whatever the file, a source hands back the same
 * thing -- interleaved signed 16-bit samples at the file's own rate and
 * channel count -- and everything after that is one pipe: what the hardware
 * takes is exactly 48 kHz, 16-bit, stereo, and the conversion is done a block
 * at a time so a long file does not have to be resampled into memory before
 * the first note.
 *
 * WAV is read from the file as it plays. MP3 and Vorbis are read into memory
 * whole -- a song is a few megabytes, and a decoder that can see all of it
 * knows exactly how long it is and can go back to the start -- and decoded a
 * block at a time from there, by minimp3 and stb_vorbis (third_party/, see
 * tools/vendor.py; both public domain).
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

/* The decoders, compiled into this program. Their own code is theirs, so
 * their warnings are theirs too. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include <alloca.h>
#ifndef alloca
#define alloca __builtin_alloca
#endif
#define get_bits stbv_get_bits      /* the toolkit has a get_bits of its own */
#include "stb_vorbis.c"
#undef get_bits
#pragma GCC diagnostic pop

#define OUT_RATE  48000
#define CHUNK     4096            /* output frames per top-up */

static char path[UPATH_MAX];
static char status[200];

/* --- sources -------------------------------------------------------------- */

enum { SRC_NONE, SRC_WAV, SRC_MP3, SRC_OGG };

static int    src_kind;
static int    src_ch;
static long   src_rate;
static long   total_frames, played_frames;   /* in the file's own frames */

/* WAV: read from the file as it plays. */
static long   fd = -1;
static int    wav_bits;
static long   data_start, data_bytes, data_pos;

/* MP3 and Vorbis: the whole file in memory, decoded from there. */
static unsigned char *filebuf;
static long           filelen;
static mp3dec_ex_t    mp3;
static stb_vorbis    *ogg;

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void src_close(void) {
    if (fd >= 0) { xyuos_close(fd); fd = -1; }
    if (src_kind == SRC_MP3) mp3dec_ex_close(&mp3);
    if (ogg) { stb_vorbis_close(ogg); ogg = NULL; }
    free(filebuf);
    filebuf = NULL;
    filelen = 0;
    src_kind = SRC_NONE;
    total_frames = played_frames = 0;
}

static int wav_open(const char *p) {
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
            src_ch   = (int)rd16(f + 2);
            src_rate = (long)rd32(f + 4);
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
    if (src_ch < 1 || src_ch > 8 || src_rate <= 0) {
        snprintf(status, sizeof status, "unsupported channel count (%d)", src_ch);
        xyuos_close(fd); fd = -1;
        return 0;
    }

    int frame_bytes = src_ch * wav_bits / 8;
    total_frames = data_bytes / frame_bytes;
    data_pos = 0;
    xyuos_seek(fd, data_start, SEEK_SET);
    src_kind = SRC_WAV;
    snprintf(status, sizeof status, "WAV, %ld Hz, %d ch, %d bit", src_rate, src_ch, wav_bits);
    return 1;
}

/* The whole file into filebuf. */
static int load_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(status, sizeof status, "cannot open %s", p); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 512L * 1024 * 1024) {
        fclose(f);
        snprintf(status, sizeof status, "file is empty or too large");
        return 0;
    }
    filebuf = (unsigned char *)malloc((size_t)n);
    if (!filebuf) { fclose(f); snprintf(status, sizeof status, "out of memory"); return 0; }
    long got = (long)fread(filebuf, 1, (size_t)n, f);
    fclose(f);
    if (got != n) {
        free(filebuf); filebuf = NULL;
        snprintf(status, sizeof status, "could not read %s", p);
        return 0;
    }
    filelen = n;
    return 1;
}

static int mp3_open(const char *p) {
    if (!load_file(p)) return 0;
    /* Seek-to-sample mode scans the frame headers once, which is what gives
     * the exact length -- a variable-bitrate file has no other honest one. */
    if (mp3dec_ex_open_buf(&mp3, filebuf, (size_t)filelen, MP3D_SEEK_TO_SAMPLE) ||
        mp3.info.channels < 1 || mp3.info.hz <= 0) {
        free(filebuf); filebuf = NULL;
        snprintf(status, sizeof status, "not an MP3 this decoder can read");
        return 0;
    }
    src_kind = SRC_MP3;
    src_ch = mp3.info.channels;
    src_rate = mp3.info.hz;
    total_frames = (long)(mp3.samples / (uint64_t)src_ch);
    snprintf(status, sizeof status, "MP3, %ld Hz, %d ch, %d kbps",
             src_rate, src_ch, mp3.info.bitrate_kbps);
    return 1;
}

static int ogg_open(const char *p) {
    if (!load_file(p)) return 0;
    int err = 0;
    ogg = stb_vorbis_open_memory(filebuf, (int)filelen, &err, NULL);
    if (!ogg) {
        free(filebuf); filebuf = NULL;
        /* An Ogg file is a container; Opus comes in one too. */
        snprintf(status, sizeof status, "not Ogg Vorbis (Opus is not supported)");
        return 0;
    }
    stb_vorbis_info info = stb_vorbis_get_info(ogg);
    src_kind = SRC_OGG;
    src_ch = info.channels;
    src_rate = (long)info.sample_rate;
    total_frames = (long)stb_vorbis_stream_length_in_samples(ogg);
    snprintf(status, sizeof status, "Ogg Vorbis, %ld Hz, %d ch", src_rate, src_ch);
    return 1;
}

static int ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++) {
        char c = s[n - e + i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != ext[i]) return 0;
    }
    return 1;
}

/* By what the file starts with, not by its name -- a .mp3 that is really a
 * WAV still plays -- and by the name only when the start says nothing. */
static int src_open(const char *p) {
    src_close();
    unsigned char m[4] = { 0, 0, 0, 0 };
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(status, sizeof status, "cannot open %s", p); return 0; }
    size_t got = fread(m, 1, 4, f);
    fclose(f);

    int ok;
    if (got == 4 && !memcmp(m, "RIFF", 4))                       ok = wav_open(p);
    else if (got == 4 && !memcmp(m, "OggS", 4))                  ok = ogg_open(p);
    else if (got >= 3 && (!memcmp(m, "ID3", 3) ||
                          (m[0] == 0xFF && (m[1] & 0xE0) == 0xE0))) ok = mp3_open(p);
    else if (ends_with(p, ".mp3"))                               ok = mp3_open(p);
    else if (ends_with(p, ".ogg") || ends_with(p, ".oga"))       ok = ogg_open(p);
    else { snprintf(status, sizeof status, "not WAV, MP3 or Ogg Vorbis"); ok = 0; }

    if (ok) {
        played_frames = 0;
        snprintf(path, sizeof path, "%s", p);
    }
    return ok;
}

/* --- decode + convert ------------------------------------------------------ */

#define PCM_MAX (CHUNK * 2 + 16)          /* input frames per block, enough up to 96 kHz */
static unsigned char raw[PCM_MAX * 8 * 4];  /* worst case: 8ch x 32-bit */
static short         pcm[PCM_MAX * 8];      /* the source's frames, as 16-bit */
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

/* Up to `frames` of the source into pcm. How many came; 0 at the end. */
static long src_read(long frames) {
    if (frames > PCM_MAX) frames = PCM_MAX;
    switch (src_kind) {
    case SRC_WAV: {
        int frame_bytes = src_ch * wav_bits / 8;
        long avail = (data_bytes - data_pos) / frame_bytes;
        if (frames > avail) frames = avail;
        if (frames <= 0) return 0;
        long got = xyuos_read(fd, raw, (unsigned long)(frames * frame_bytes));
        if (got <= 0) return 0;
        long n = got / frame_bytes;
        data_pos += n * frame_bytes;
        for (long i = 0; i < n * src_ch; i++)
            pcm[i] = (short)sample_at(raw + i * (wav_bits / 8), wav_bits);
        return n;
    }
    case SRC_MP3: {
        size_t got = mp3dec_ex_read(&mp3, pcm, (size_t)(frames * src_ch));
        return (long)(got / (size_t)src_ch);
    }
    case SRC_OGG:
        return stb_vorbis_get_samples_short_interleaved(ogg, src_ch, pcm,
                                                        (int)(frames * src_ch));
    default:
        return 0;
    }
}

/* Fill `out` with up to `want` output frames. Returns how many were produced;
 * 0 means the file is finished. */
static int decode(int want) {
    if (src_kind == SRC_NONE) return 0;
    /* How many INPUT frames this many output frames needs. */
    long need_in = ((long)want * src_rate + OUT_RATE - 1) / OUT_RATE + 1;
    long in_frames = src_read(need_in);
    if (in_frames <= 0) return 0;

    int produced = 0;
    for (int i = 0; i < want; i++) {
        long src = (long)i * src_rate / OUT_RATE;
        if (src >= in_frames) break;
        const short *f = pcm + src * src_ch;
        out[produced * 2 + 0] = f[0];
        out[produced * 2 + 1] = src_ch > 1 ? f[1] : f[0];
        produced++;
    }
    played_frames += in_frames;
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
        long secs = src_rate ? played_frames / src_rate : 0;
        long tot  = src_rate ? total_frames / src_rate : 0;
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
    switch (src_kind) {
    case SRC_WAV: xyuos_seek(fd, data_start, SEEK_SET); data_pos = 0; break;
    case SRC_MP3: mp3dec_ex_seek(&mp3, 0); break;
    case SRC_OGG: stb_vorbis_seek_start(ogg); break;
    default: return;
    }
    played_frames = 0;
}

static void do_button(int i) {
    switch (i) {
        case 0:
            if (src_kind == SRC_NONE) { snprintf(status, sizeof status, "nothing loaded"); break; }
            if (played_frames >= total_frames) restart();
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
        if (src_open(abs)) playing = 1;
        else message_box(MSG_ERROR, "PLAY-OPEN", "Cannot play this file",
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
                if (ends_with(name, ".wav") || ends_with(name, ".mp3") ||
                    ends_with(name, ".ogg"))
                    upath_resolve("/sounds", name, pick);
            }
            i = j + 1;
        }
        if (pick[0] && src_open(pick)) playing = 1;
        else snprintf(status, sizeof status, "usage: play <file.wav|mp3|ogg>");
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
    src_close();
    gui_close(&g);
    return 0;
}
