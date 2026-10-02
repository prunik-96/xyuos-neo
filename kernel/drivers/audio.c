#include "audio.h"
#include "pci.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../kernel/kio.h"
#include "../include/port_io.h"
#include "../arch/x86_64/pit.h"
#include <stddef.h>

/* Intel High Definition Audio.
 *
 * The controller and the codec are two different machines that barely know
 * about each other. The controller does DMA: you hand it a scatter list and a
 * stream number and it pushes bytes at a fixed rate forever. The codec is a
 * little tree of widgets -- converters, mixers, pins -- reached only by
 * sending four-byte "verbs" down a ring buffer and reading four-byte replies
 * out of another one. Neither half makes a sound on its own: the controller
 * has to be told which stream number it is playing, and something inside the
 * codec has to be told to listen for that same number and route it to a
 * socket that has a speaker in it.
 *
 * Almost all the code below is that second half -- finding a path from a
 * digital-to-analogue converter to a jack that is actually plugged into
 * something, and switching on every amplifier in between. The DMA is twenty
 * lines. */

/* --- controller registers (BAR0) ---------------------------------------- */
#define HDA_GCAP        0x00   /* 16 */
#define HDA_GCTL        0x08   /* 32 */
#define HDA_WAKEEN      0x0C   /* 16 */
#define HDA_STATESTS    0x0E   /* 16 */
#define HDA_INTCTL      0x20   /* 32 */
#define HDA_CORBLBASE   0x40
#define HDA_CORBUBASE   0x44
#define HDA_CORBWP      0x48   /* 16 */
#define HDA_CORBRP      0x4A   /* 16 */
#define HDA_CORBCTL     0x4C   /* 8  */
#define HDA_CORBSIZE    0x4E   /* 8  */
#define HDA_RIRBLBASE   0x50
#define HDA_RIRBUBASE   0x54
#define HDA_RIRBWP      0x58   /* 16 */
#define HDA_RINTCNT     0x5A   /* 16 */
#define HDA_RIRBCTL     0x5C   /* 8  */
#define HDA_RIRBSTS     0x5D   /* 8  */
#define HDA_RIRBSIZE    0x5E   /* 8  */
#define HDA_SD_BASE     0x80
#define HDA_SD_STRIDE   0x20

/* stream descriptor, relative to its base */
#define SD_CTL          0x00
#define SD_STS          0x03
#define SD_LPIB         0x04
#define SD_CBL          0x08
#define SD_LVI          0x0C
#define SD_FMT          0x12
#define SD_BDPL         0x18
#define SD_BDPU         0x1C

/* --- codec verbs --------------------------------------------------------- */
#define VERB_GET_PARAM        0xF00
#define VERB_GET_CONN_LIST    0xF02
#define VERB_SET_CONN_SEL     0x701
#define VERB_GET_CONN_SEL     0xF01
#define VERB4_STREAM_FMT      0x2    /* 4-bit opcode, 16-bit payload */
#define VERB4_AMP_GAIN        0x3
#define VERB_SET_STREAM_CHAN  0x706
#define VERB_SET_PIN_CTL      0x707
#define VERB_GET_PIN_CTL      0xF07
#define VERB_SET_EAPD         0x70C
#define VERB_SET_POWER        0x705
#define VERB_GET_CONFIG_DEF   0xF1C

#define PARAM_NODE_COUNT      0x04
#define PARAM_FUNC_TYPE       0x05
#define PARAM_WIDGET_CAP      0x09
#define PARAM_PIN_CAP         0x0C
#define PARAM_CONN_LIST_LEN   0x0E

#define WT_DAC   0
#define WT_ADC   1
#define WT_MIXER 2
#define WT_SEL   3
#define WT_PIN   4

#define STREAM_TAG 1
#define BDL_COUNT  4
#define BUF_BYTES  (64 * 1024)            /* 64 KB ~ 170 ms of 48k stereo */
#define SEG_BYTES  (BUF_BYTES / BDL_COUNT)
#define GUARD      2048                   /* silence kept ahead of the writer */

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc;
} bdl_entry_t;

static volatile uint8_t *mmio;
static volatile uint32_t *corb;
static volatile uint64_t *rirb;
static bdl_entry_t      *bdl;
static uint8_t          *buf;             /* the DMA ring the codec reads */
static int               corb_wp;
static int               rirb_rp;
static int               sd_off;          /* the output stream descriptor */
static int               codec_addr = -1;
static int               dac_nid, pin_nid;
static int               have_hda;
static int               wpos;            /* our write cursor into buf */
/* The same two places, counted in bytes since the stream started rather than
 * round the ring: what has been written, and what the codec has played. With
 * those a system sound can be given a place in time, and mixed in wherever
 * that place is -- over music already queued, or under music still to come. */
static uint64_t          w_abs, p_abs;
static int               last_lp;
static int               cleared;         /* played bytes are silenced up to here */
static int               volume = 80;

/* --- MMIO helpers -------------------------------------------------------- */
static inline uint8_t  r8 (int o)  { return *(volatile uint8_t  *)(mmio + o); }
static inline uint16_t r16(int o)  { return *(volatile uint16_t *)(mmio + o); }
static inline uint32_t r32(int o)  { return *(volatile uint32_t *)(mmio + o); }
static inline void w8 (int o, uint8_t v)  { *(volatile uint8_t  *)(mmio + o) = v; }
static inline void w16(int o, uint16_t v) { *(volatile uint16_t *)(mmio + o) = v; }
static inline void w32(int o, uint32_t v) { *(volatile uint32_t *)(mmio + o) = v; }

static void spin(int n) {
    for (volatile int i = 0; i < n * 2000; i++) __asm__ volatile ("pause");
}

static void *alloc_contig(uint32_t bytes) {
    uint32_t pages = (bytes + 4095) / 4096;
    uint64_t start = pmm_alloc_contig(pages);
    if (!start) return NULL;
    // Frames come back as whoever had them left them. For the sound ring
    // that was audible: whatever lay there played twice at start-up, a click
    // a lap (340 ms) apart, before the first tick began silencing it.
    uint8_t *p = (uint8_t *)(uintptr_t)start;
    for (uint32_t i = 0; i < pages * 4096; i++) p[i] = 0;
    return p;
}

/* --- the verb ring -------------------------------------------------------
 * Send one command, wait for the one reply. Synchronous on purpose: codec
 * setup happens once at boot and is a few dozen verbs, so there is nothing to
 * gain from pipelining and a great deal to lose in complexity. */
static uint32_t codec_raw(int nid, uint32_t verb20) {
    if (codec_addr < 0) return 0;
    uint32_t val = ((uint32_t)codec_addr << 28) | ((uint32_t)nid << 20) |
                   (verb20 & 0xFFFFF);

    corb_wp = (corb_wp + 1) % 256;
    corb[corb_wp] = val;
    __asm__ volatile ("sfence" ::: "memory");
    w16(HDA_CORBWP, (uint16_t)corb_wp);

    for (int i = 0; i < 4000; i++) {
        int wp = r16(HDA_RIRBWP) & 0xFF;
        if (wp != rirb_rp) {
            rirb_rp = (rirb_rp + 1) % 256;
            /* A RIRB entry is 8 bytes: the 32-bit response plus 32 bits
             * of response-extended. One uint64_t per entry, not two. */
            uint64_t resp = rirb[rirb_rp];
            w8(HDA_RIRBSTS, 0x05);          /* ack */
            return (uint32_t)resp;
        }
        spin(1);
    }
    return 0;
}

/* 12-bit opcode, 8-bit payload: the common form. */
static uint32_t codec_cmd(int nid, uint32_t verb, uint32_t payload) {
    return codec_raw(nid, (verb << 8) | (payload & 0xFF));
}

/* 4-bit opcode, 16-bit payload: stream format and amplifier gain. */
static uint32_t codec_cmd16(int nid, uint32_t verb4, uint32_t payload) {
    return codec_raw(nid, (verb4 << 16) | (payload & 0xFFFF));
}

static uint32_t param(int nid, uint32_t which) {
    return codec_cmd(nid, VERB_GET_PARAM, which);
}

/* --- finding a way out of the codec --------------------------------------
 * Walk back from a pin towards a converter. The tree is shallow (pin ->
 * maybe a mixer or selector -> DAC), so a depth limit of four is generous,
 * and it stops a malformed connection list from looping forever. */
static int trace_to_dac(int nid, int depth, int *sel_of, int *sel_idx) {
    if (depth > 4) return -1;
    uint32_t cap = param(nid, PARAM_WIDGET_CAP);
    int type = (int)((cap >> 20) & 0xF);
    if (type == WT_DAC) return nid;

    int len = (int)(param(nid, PARAM_CONN_LIST_LEN) & 0x7F);
    for (int i = 0; i < len && i < 16; i++) {
        uint32_t list = codec_cmd(nid, VERB_GET_CONN_LIST, (uint32_t)(i & ~3));
        int entry = (int)((list >> (8 * (i & 3))) & 0xFF);
        if (!entry) continue;
        int found = trace_to_dac(entry, depth + 1, sel_of, sel_idx);
        if (found >= 0) {
            /* Remember the branch to take, so the selector can be pointed at
             * it once the whole path is known to be good. */
            if (*sel_of < 0) { *sel_of = nid; *sel_idx = i; }
            return found;
        }
    }
    return -1;
}

/* Open a widget's output amplifier all the way.
 *
 * The gain range is per-widget: a codec that offers 64 steps and one that
 * offers 8 both exist, and writing a constant means "loud" on one and
 * "clipped" or "silent" on the other. So ask, then use the top of whatever
 * range came back -- the master volume is applied in software anyway.
 * 0xB000 = set the OUTPUT amp, left and right, unmuted. */
#define PARAM_AMP_OUT_CAP 0x12

static void unmute(int nid) {
    uint32_t cap = param(nid, PARAM_AMP_OUT_CAP);
    uint32_t steps = (cap >> 8) & 0x7F;
    if (!steps) steps = 0x4F;                    /* no capability reported */
    codec_cmd16(nid, VERB4_AMP_GAIN, 0xB000u | (steps & 0x7F));
}

static int setup_codec(void) {
    /* Function groups hang off the root node. */
    uint32_t sub = param(0, PARAM_NODE_COUNT);
    int fg_start = (int)((sub >> 16) & 0xFF), fg_count = (int)(sub & 0xFF);

    int afg = -1;
    for (int i = 0; i < fg_count; i++) {
        int nid = fg_start + i;
        if ((param(nid, PARAM_FUNC_TYPE) & 0xFF) == 0x01) { afg = nid; break; }
    }
    if (afg < 0) return 0;

    codec_cmd(afg, VERB_SET_POWER, 0);      /* D0 */
    spin(20);

    sub = param(afg, PARAM_NODE_COUNT);
    int w_start = (int)((sub >> 16) & 0xFF), w_count = (int)(sub & 0xFF);

    /* Prefer a jack that something is plugged into, then a built-in speaker,
     * then anything that can drive an output at all. A machine with no
     * headphones in it must still be able to beep. */
    int best_pin = -1, best_rank = -1;
    for (int i = 0; i < w_count; i++) {
        int nid = w_start + i;
        uint32_t cap = param(nid, PARAM_WIDGET_CAP);
        if (((cap >> 20) & 0xF) != WT_PIN) continue;
        uint32_t pcap = param(nid, PARAM_PIN_CAP);
        if (!(pcap & (1u << 4))) continue;              /* cannot output */

        uint32_t cfg = codec_cmd(nid, VERB_GET_CONFIG_DEF, 0);
        int dev  = (int)((cfg >> 20) & 0xF);            /* 0 line out, 1 spk, 2 hp */
        int conn = (int)((cfg >> 30) & 0x3);            /* 3 = not connected */
        if (conn == 3) continue;

        int rank = (dev == 2) ? 3 : (dev == 0) ? 2 : (dev == 1) ? 1 : 0;
        if (rank > best_rank) { best_rank = rank; best_pin = nid; }
    }
    if (best_pin < 0) {
        for (int i = 0; i < w_count; i++) {
            int nid = w_start + i;
            uint32_t cap = param(nid, PARAM_WIDGET_CAP);
            if (((cap >> 20) & 0xF) != WT_PIN) continue;
            if (param(nid, PARAM_PIN_CAP) & (1u << 4)) { best_pin = nid; break; }
        }
    }
    if (best_pin < 0) return 0;

    int sel_of = -1, sel_idx = 0;
    int dac = trace_to_dac(best_pin, 0, &sel_of, &sel_idx);
    if (dac < 0) {
        for (int i = 0; i < w_count; i++) {             /* fall back: any DAC */
            int nid = w_start + i;
            if (((param(nid, PARAM_WIDGET_CAP) >> 20) & 0xF) == WT_DAC) { dac = nid; break; }
        }
    }
    if (dac < 0) return 0;

    pin_nid = best_pin;
    dac_nid = dac;

    codec_cmd(pin_nid, VERB_SET_POWER, 0);
    codec_cmd(dac_nid, VERB_SET_POWER, 0);
    spin(20);

    /* Pin: output enable + headphone drive. */
    codec_cmd(pin_nid, VERB_SET_PIN_CTL, 0x40 | 0x80);
    codec_cmd(pin_nid, VERB_SET_EAPD, 0x02);            /* external amp on */
    if (sel_of >= 0) codec_cmd(sel_of, VERB_SET_CONN_SEL, (uint32_t)sel_idx);

    unmute(pin_nid);
    unmute(dac_nid);
    if (sel_of >= 0 && sel_of != pin_nid) unmute(sel_of);

    /* 48 kHz, 16-bit, stereo -- see the FMT field layout in the spec. */
    codec_cmd16(dac_nid, VERB4_STREAM_FMT, 0x0011);
    codec_cmd(dac_nid, VERB_SET_STREAM_CHAN, (STREAM_TAG << 4) | 0);

    kprintf("hda: codec %d, dac nid %d, pin nid %d\n", codec_addr, dac_nid, pin_nid);
    return 1;
}

static void stream_start(void) {
    w8(sd_off + SD_CTL, 0x01);                          /* SRST */
    for (int i = 0; i < 200 && !(r8(sd_off + SD_CTL) & 1); i++) spin(1);
    w8(sd_off + SD_CTL, 0x00);
    for (int i = 0; i < 200 && (r8(sd_off + SD_CTL) & 1); i++) spin(1);

    w32(sd_off + SD_CBL, BUF_BYTES);
    w16(sd_off + SD_LVI, BDL_COUNT - 1);
    w16(sd_off + SD_FMT, 0x0011);
    w32(sd_off + SD_BDPL, (uint32_t)((uint64_t)(uintptr_t)bdl & 0xFFFFFFFF));
    w32(sd_off + SD_BDPU, (uint32_t)((uint64_t)(uintptr_t)bdl >> 32));
    w8(sd_off + SD_STS, 0x1C);                          /* clear status */

    /* Stream number goes in bits 23:20 of CTL, i.e. the high nibble of byte 2. */
    w8(sd_off + SD_CTL + 2, (uint8_t)(STREAM_TAG << 4));
    w8(sd_off + SD_CTL, 0x02);                          /* RUN */
}

/* ==========================================================================
 * PC speaker fallback
 * ========================================================================== */

static struct { uint16_t hz; uint16_t ms; } melody[8];
static int melody_len, melody_at;
static uint64_t melody_until;
static int speaker_on;

static void speaker_tone(uint16_t hz) {
    if (!hz) {
        outb(0x61, (uint8_t)(inb(0x61) & 0xFC));
        speaker_on = 0;
        return;
    }
    uint32_t div = 1193182u / hz;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    outb(0x61, (uint8_t)(inb(0x61) | 0x03));
    speaker_on = 1;
}

/* Silence what the codec has already played. The ring is 340 ms long and
 * the codec goes round it for as long as the stream runs, so audio that is
 * left in it plays again on every lap: a track that ended kept sounding its
 * last third of a second over and over until the player was closed. The
 * guard in audio_write covers only an underrun of a few milliseconds. What
 * lies behind the play position has been heard and cannot be anything the
 * writer is still waiting to have played -- the writer only ever fills the
 * part ahead of it -- so it is safe to zero, and after one lap an idle ring
 * is nothing but silence. Called every tick, under the kernel lock, as
 * audio_write is. */
static int play_pos(void);

/* Bring p_abs up to where the codec is. Called at least every tick: the ring
 * is a third of a second, so nothing is missed. */
static uint64_t last_update_ms;
static void play_update(void) {
    int lp = play_pos();
    int d = lp - last_lp;
    if (d < 0) d += BUF_BYTES;
    // The ring position alone cannot tell one lap from three. If nobody
    // looked for longer than a lap -- interrupts off through a long piece of
    // start-up -- the clock says how many went by.
    uint64_t now = pit_get_ticks() * 10;
    if (last_update_ms) {
        uint64_t expect = (now - last_update_ms) * (uint64_t)(AUDIO_RATE / 1000 * 2 * AUDIO_CH);
        while ((uint64_t)d + BUF_BYTES / 2 < expect && d + BUF_BYTES > d) d += BUF_BYTES;
    }
    last_update_ms = now;
    p_abs += (uint64_t)d;
    last_lp = lp;
}

static void fx_fill(void);

static void silence_played(void) {
    int lp = play_pos();
    int n = lp - cleared;
    if (n < 0) n += BUF_BYTES;
    for (int i = 0; i < n; i++) buf[(cleared + i) % BUF_BYTES] = 0;
    cleared = lp;
}

void audio_tick(void) {
    if (have_hda) { silence_played(); fx_fill(); }
    if (!melody_len) return;
    if (pit_get_ticks() * 10 < melody_until) return;
    if (melody_at >= melody_len) {
        speaker_tone(0);
        melody_len = melody_at = 0;
        return;
    }
    speaker_tone(melody[melody_at].hz);
    melody_until = pit_get_ticks() * 10 + melody[melody_at].ms;
    melody_at++;
}

/* ==========================================================================
 * public interface
 * ========================================================================== */

int audio_ready(void) { return have_hda; }
const char *audio_backend(void) { return have_hda ? "Intel HD Audio" : "PC speaker"; }
int audio_volume(void) { return volume; }
void audio_set_volume(int pct) {
    volume = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

static char now_title[AUDIO_TITLE_MAX];

void audio_set_title(const char *t) {
    int i = 0;
    while (t && t[i] && i < AUDIO_TITLE_MAX - 1) { now_title[i] = t[i]; i++; }
    now_title[i] = 0;
}

// Only while something is actually coming out: a player that was killed
// never got to say it stopped, but its sound runs dry all the same.
const char *audio_title(void) {
    return audio_queued() > 0 ? now_title : "";
}

int audio_init(void) {
    pci_device_t dev;
    if (!pci_find_class(0x04, 0x03, 0x00, &dev)) {
        kprintf("hda: no controller; sounds go to the PC speaker\n");
        return 0;
    }
    pci_enable_bus_master(&dev);

    uint64_t bar = dev.bar[0] & ~0xFULL;
    if ((dev.bar[0] & 0x6) == 0x4)               /* 64-bit BAR */
        bar |= (uint64_t)dev.bar[1] << 32;
    if (!bar) return 0;
    paging_map_mmio(bar, 0x4000);
    mmio = (volatile uint8_t *)(uintptr_t)bar;

    /* Reset: drop CRST, wait for it to read back low, raise it again. */
    w32(HDA_GCTL, r32(HDA_GCTL) & ~1u);
    for (int i = 0; i < 500 && (r32(HDA_GCTL) & 1); i++) spin(1);
    w32(HDA_GCTL, r32(HDA_GCTL) | 1u);
    for (int i = 0; i < 500 && !(r32(HDA_GCTL) & 1); i++) spin(1);
    spin(60);                                    /* codecs need ~521 us */

    uint16_t states = r16(HDA_STATESTS);
    for (int i = 0; i < 15; i++) if (states & (1u << i)) { codec_addr = i; break; }
    if (codec_addr < 0) {
        kprintf("hda: controller present but no codec answered\n");
        return 0;
    }

    corb = (volatile uint32_t *)alloc_contig(1024);
    rirb = (volatile uint64_t *)alloc_contig(2048);
    bdl  = (bdl_entry_t *)alloc_contig(4096);
    buf  = (uint8_t *)alloc_contig(BUF_BYTES);
    if (!corb || !rirb || !bdl || !buf) { kprintf("hda: out of DMA memory\n"); return 0; }

    w8(HDA_CORBCTL, 0);
    w8(HDA_RIRBCTL, 0);
    w8(HDA_CORBSIZE, 0x02);                      /* 256 entries */
    w8(HDA_RIRBSIZE, 0x02);
    w32(HDA_CORBLBASE, (uint32_t)((uint64_t)(uintptr_t)corb & 0xFFFFFFFF));
    w32(HDA_CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)corb >> 32));
    w32(HDA_RIRBLBASE, (uint32_t)((uint64_t)(uintptr_t)rirb & 0xFFFFFFFF));
    w32(HDA_RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)rirb >> 32));

    w16(HDA_CORBRP, 0x8000);                     /* reset the read pointer */
    for (int i = 0; i < 200 && !(r16(HDA_CORBRP) & 0x8000); i++) spin(1);
    w16(HDA_CORBRP, 0);
    for (int i = 0; i < 200 && (r16(HDA_CORBRP) & 0x8000); i++) spin(1);
    w16(HDA_CORBWP, 0);
    corb_wp = 0;

    w16(HDA_RIRBWP, 0x8000);
    w16(HDA_RINTCNT, 1);
    rirb_rp = 0;

    w8(HDA_CORBCTL, 0x02);                       /* CORB DMA run */
    /* RIRBCTL bit 1 is the DMA engine; bit 0 makes the controller RAISE the
     * response-status flag when RINTCNT replies have landed. That flag looks
     * like an interrupt thing and is easy to leave off when polling -- but the
     * controller also stops fetching commands until the flag is CLEARED, and a
     * flag that is never set can never be cleared. Without bit 0 exactly one
     * verb works and the ring wedges forever. */
    w8(HDA_RIRBCTL, 0x03);
    spin(10);

    uint16_t gcap = r16(HDA_GCAP);
    int iss = (gcap >> 8) & 0xF;
    sd_off = HDA_SD_BASE + iss * HDA_SD_STRIDE;  /* output streams follow inputs */

    for (int i = 0; i < BDL_COUNT; i++) {
        bdl[i].addr = (uint64_t)(uintptr_t)(buf + i * SEG_BYTES);
        bdl[i].len  = SEG_BYTES;
        bdl[i].ioc  = 0;
    }

    if (!setup_codec()) {
        kprintf("hda: no usable output path in the codec\n");
        return 0;
    }

    stream_start();
    have_hda = 1;
    wpos = 0;
    cleared = 0;
    w_abs = p_abs = 0;
    last_lp = play_pos();
    kprintf("hda: ready, 48 kHz stereo\n");
    return 1;
}

static int play_pos(void) {
    return (int)(r32(sd_off + SD_LPIB) % BUF_BYTES);
}

int audio_queued(void) {
    if (!have_hda) return 0;
    play_update();
    return w_abs > p_abs ? (int)((w_abs - p_abs) / (2 * AUDIO_CH)) : 0;
}

static void fx_mix(uint64_t a, uint64_t b);

int audio_write(const int16_t *frames, int nframes) {
    if (!have_hda || nframes <= 0) return 0;

    play_update();
    // Fallen behind -- the codec has played past what was written: go on
    // from just ahead of it, not from where it will come round to in a lap.
    if (w_abs < p_abs + 4 * AUDIO_CH * 48) {
        if (w_abs < p_abs) {
            w_abs = p_abs + 4 * AUDIO_CH * 48;      // 1 ms ahead
            wpos = (int)(w_abs % BUF_BYTES);
        }
    }
    int used = (int)(w_abs - p_abs);
    int free_bytes = BUF_BYTES - used - GUARD;
    if (free_bytes <= 0) return 0;

    int want = nframes * 2 * AUDIO_CH;
    if (want > free_bytes) want = free_bytes & ~3;
    if (want <= 0) return 0;

    const uint8_t *src = (const uint8_t *)frames;
    int vol = volume;
    for (int i = 0; i < want; i += 2) {
        int16_t s = (int16_t)((uint16_t)src[i] | ((uint16_t)src[i + 1] << 8));
        int scaled = (int)s * vol / 100;
        int at = (wpos + i) % BUF_BYTES;
        buf[at] = (uint8_t)(scaled & 0xFF);
        buf[(at + 1) % BUF_BYTES] = (uint8_t)((scaled >> 8) & 0xFF);
    }
    uint64_t from = w_abs;
    w_abs += (uint64_t)want;
    wpos = (int)(w_abs % BUF_BYTES);
    fx_mix(from, w_abs);                 /* a system sound over this stretch */

    /* Leave silence in front of the writer. Without this, an underrun replays
     * whatever was in the ring last time round, which is far more alarming
     * than a gap. */
    for (int i = 0; i < GUARD; i++) buf[(wpos + i) % BUF_BYTES] = 0;

    __asm__ volatile ("sfence" ::: "memory");
    return want / (2 * AUDIO_CH);
}

void audio_stop(void) {
    if (!have_hda) return;
    for (int i = 0; i < BUF_BYTES; i++) buf[i] = 0;
    play_update();
    w_abs = p_abs;
    wpos = play_pos();
    cleared = wpos;
}

/* --- system sounds --------------------------------------------------------
 * Soft bells, synthesised when asked for: a few notes, each a sine with two
 * overtones, a quick rise and an exponential fall -- the higher overtones
 * falling faster, as a struck bar's do. Integer arithmetic only: this runs
 * wherever the sound is asked for, inside the kernel.
 *
 * A sound is rendered whole into fx_pcm (mono), given a place a little ahead
 * of what the codec is playing, and mixed into the ring: at once over music
 * already queued, by audio_write over music still to come, and by fx_fill on
 * the tick when nothing is playing. */

static const int16_t sine_tab[1024] = {
    0, 201, 402, 603, 804, 1005, 1206, 1407, 1608, 1809, 2009, 2210, 2410, 2611, 2811, 3012,
    3212, 3412, 3612, 3811, 4011, 4210, 4410, 4609, 4808, 5007, 5205, 5404, 5602, 5800, 5998, 6195,
    6393, 6590, 6786, 6983, 7179, 7375, 7571, 7767, 7962, 8157, 8351, 8545, 8739, 8933, 9126, 9319,
    9512, 9704, 9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605, 11793, 11980, 12167, 12353,
    12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828, 14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269,
    15446, 15623, 15800, 15976, 16151, 16325, 16499, 16673, 16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000, 20159, 20317, 20475, 20631,
    20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856, 22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027,
    23170, 23311, 23452, 23592, 23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674, 26790, 26905, 27019, 27133,
    27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001, 28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803,
    28898, 28992, 29085, 29177, 29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050, 31113, 31176, 31237, 31297,
    31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736, 31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098,
    32137, 32176, 32213, 32250, 32285, 32318, 32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737, 32745, 32752, 32757, 32761, 32765, 32766,
    32767, 32766, 32765, 32761, 32757, 32752, 32745, 32737, 32728, 32717, 32705, 32692, 32678, 32663, 32646, 32628,
    32609, 32589, 32567, 32545, 32521, 32495, 32469, 32441, 32412, 32382, 32351, 32318, 32285, 32250, 32213, 32176,
    32137, 32098, 32057, 32014, 31971, 31926, 31880, 31833, 31785, 31736, 31685, 31633, 31580, 31526, 31470, 31414,
    31356, 31297, 31237, 31176, 31113, 31050, 30985, 30919, 30852, 30783, 30714, 30643, 30571, 30498, 30424, 30349,
    30273, 30195, 30117, 30037, 29956, 29874, 29791, 29706, 29621, 29534, 29447, 29358, 29268, 29177, 29085, 28992,
    28898, 28803, 28706, 28609, 28510, 28411, 28310, 28208, 28105, 28001, 27896, 27790, 27683, 27575, 27466, 27356,
    27245, 27133, 27019, 26905, 26790, 26674, 26556, 26438, 26319, 26198, 26077, 25955, 25832, 25708, 25582, 25456,
    25329, 25201, 25072, 24942, 24811, 24680, 24547, 24413, 24279, 24143, 24007, 23870, 23731, 23592, 23452, 23311,
    23170, 23027, 22884, 22739, 22594, 22448, 22301, 22154, 22005, 21856, 21705, 21554, 21403, 21250, 21096, 20942,
    20787, 20631, 20475, 20317, 20159, 20000, 19841, 19680, 19519, 19357, 19195, 19032, 18868, 18703, 18537, 18371,
    18204, 18037, 17869, 17700, 17530, 17360, 17189, 17018, 16846, 16673, 16499, 16325, 16151, 15976, 15800, 15623,
    15446, 15269, 15090, 14912, 14732, 14553, 14372, 14191, 14010, 13828, 13645, 13462, 13279, 13094, 12910, 12725,
    12539, 12353, 12167, 11980, 11793, 11605, 11417, 11228, 11039, 10849, 10659, 10469, 10278, 10087, 9896, 9704,
    9512, 9319, 9126, 8933, 8739, 8545, 8351, 8157, 7962, 7767, 7571, 7375, 7179, 6983, 6786, 6590,
    6393, 6195, 5998, 5800, 5602, 5404, 5205, 5007, 4808, 4609, 4410, 4210, 4011, 3811, 3612, 3412,
    3212, 3012, 2811, 2611, 2410, 2210, 2009, 1809, 1608, 1407, 1206, 1005, 804, 603, 402, 201,
    0, -201, -402, -603, -804, -1005, -1206, -1407, -1608, -1809, -2009, -2210, -2410, -2611, -2811, -3012,
    -3212, -3412, -3612, -3811, -4011, -4210, -4410, -4609, -4808, -5007, -5205, -5404, -5602, -5800, -5998, -6195,
    -6393, -6590, -6786, -6983, -7179, -7375, -7571, -7767, -7962, -8157, -8351, -8545, -8739, -8933, -9126, -9319,
    -9512, -9704, -9896, -10087, -10278, -10469, -10659, -10849, -11039, -11228, -11417, -11605, -11793, -11980, -12167, -12353,
    -12539, -12725, -12910, -13094, -13279, -13462, -13645, -13828, -14010, -14191, -14372, -14553, -14732, -14912, -15090, -15269,
    -15446, -15623, -15800, -15976, -16151, -16325, -16499, -16673, -16846, -17018, -17189, -17360, -17530, -17700, -17869, -18037,
    -18204, -18371, -18537, -18703, -18868, -19032, -19195, -19357, -19519, -19680, -19841, -20000, -20159, -20317, -20475, -20631,
    -20787, -20942, -21096, -21250, -21403, -21554, -21705, -21856, -22005, -22154, -22301, -22448, -22594, -22739, -22884, -23027,
    -23170, -23311, -23452, -23592, -23731, -23870, -24007, -24143, -24279, -24413, -24547, -24680, -24811, -24942, -25072, -25201,
    -25329, -25456, -25582, -25708, -25832, -25955, -26077, -26198, -26319, -26438, -26556, -26674, -26790, -26905, -27019, -27133,
    -27245, -27356, -27466, -27575, -27683, -27790, -27896, -28001, -28105, -28208, -28310, -28411, -28510, -28609, -28706, -28803,
    -28898, -28992, -29085, -29177, -29268, -29358, -29447, -29534, -29621, -29706, -29791, -29874, -29956, -30037, -30117, -30195,
    -30273, -30349, -30424, -30498, -30571, -30643, -30714, -30783, -30852, -30919, -30985, -31050, -31113, -31176, -31237, -31297,
    -31356, -31414, -31470, -31526, -31580, -31633, -31685, -31736, -31785, -31833, -31880, -31926, -31971, -32014, -32057, -32098,
    -32137, -32176, -32213, -32250, -32285, -32318, -32351, -32382, -32412, -32441, -32469, -32495, -32521, -32545, -32567, -32589,
    -32609, -32628, -32646, -32663, -32678, -32692, -32705, -32717, -32728, -32737, -32745, -32752, -32757, -32761, -32765, -32766,
    -32767, -32766, -32765, -32761, -32757, -32752, -32745, -32737, -32728, -32717, -32705, -32692, -32678, -32663, -32646, -32628,
    -32609, -32589, -32567, -32545, -32521, -32495, -32469, -32441, -32412, -32382, -32351, -32318, -32285, -32250, -32213, -32176,
    -32137, -32098, -32057, -32014, -31971, -31926, -31880, -31833, -31785, -31736, -31685, -31633, -31580, -31526, -31470, -31414,
    -31356, -31297, -31237, -31176, -31113, -31050, -30985, -30919, -30852, -30783, -30714, -30643, -30571, -30498, -30424, -30349,
    -30273, -30195, -30117, -30037, -29956, -29874, -29791, -29706, -29621, -29534, -29447, -29358, -29268, -29177, -29085, -28992,
    -28898, -28803, -28706, -28609, -28510, -28411, -28310, -28208, -28105, -28001, -27896, -27790, -27683, -27575, -27466, -27356,
    -27245, -27133, -27019, -26905, -26790, -26674, -26556, -26438, -26319, -26198, -26077, -25955, -25832, -25708, -25582, -25456,
    -25329, -25201, -25072, -24942, -24811, -24680, -24547, -24413, -24279, -24143, -24007, -23870, -23731, -23592, -23452, -23311,
    -23170, -23027, -22884, -22739, -22594, -22448, -22301, -22154, -22005, -21856, -21705, -21554, -21403, -21250, -21096, -20942,
    -20787, -20631, -20475, -20317, -20159, -20000, -19841, -19680, -19519, -19357, -19195, -19032, -18868, -18703, -18537, -18371,
    -18204, -18037, -17869, -17700, -17530, -17360, -17189, -17018, -16846, -16673, -16499, -16325, -16151, -15976, -15800, -15623,
    -15446, -15269, -15090, -14912, -14732, -14553, -14372, -14191, -14010, -13828, -13645, -13462, -13279, -13094, -12910, -12725,
    -12539, -12353, -12167, -11980, -11793, -11605, -11417, -11228, -11039, -10849, -10659, -10469, -10278, -10087, -9896, -9704,
    -9512, -9319, -9126, -8933, -8739, -8545, -8351, -8157, -7962, -7767, -7571, -7375, -7179, -6983, -6786, -6590,
    -6393, -6195, -5998, -5800, -5602, -5404, -5205, -5007, -4808, -4609, -4410, -4210, -4011, -3811, -3612, -3412,
    -3212, -3012, -2811, -2611, -2410, -2210, -2009, -1809, -1608, -1407, -1206, -1005, -804, -603, -402, -201,
};

#define FX_MAX     (AUDIO_RATE * 2)       /* two seconds */
#define FX_LEAD    (AUDIO_RATE / 100 * 4) /* starts 10 ms ahead of the codec */
#define FX_AHEAD   (AUDIO_RATE / 8 * 4)   /* written 125 ms ahead when alone */
static int16_t fx_pcm[FX_MAX];
static int32_t fx_acc[FX_MAX];
static int fx_len;                        /* frames left to place, 0 = none */
static uint64_t fx_abs0, fx_done;         /* where it starts; mixed up to */

typedef struct { uint16_t at_ms, hz, tau_ms, amp; } note_t;

/* One note, added into fx_acc. */
static void bell(const note_t *n) {
    int start = n->at_ms * (AUDIO_RATE / 1000);
    int tau = n->tau_ms * (AUDIO_RATE / 1000);
    if (tau < 64) tau = 64;
    int len = tau * 6;                    /* by six time constants it is gone */
    if (start + len > FX_MAX) len = FX_MAX - start;
    if (len <= 0) return;
    /* Three partials: the note, its octave, its twelfth, quieter and shorter. */
    static const int rel[3] = { 1, 2, 3 }, gain[3] = { 1000, 280, 90 };
    for (int k = 0; k < 3; k++) {
        uint32_t step = (uint32_t)(((uint64_t)n->hz * rel[k] << 32) / AUDIO_RATE);
        if (n->hz * rel[k] >= AUDIO_RATE / 2) continue;
        int t = tau / rel[k];
        if (t < 32) t = 32;
        uint32_t keep = (1u << 24) - (1u << 24) / (uint32_t)t;   /* per frame */
        uint32_t env = 1u << 24;
        int attack = AUDIO_RATE / 250;    /* 4 ms */
        uint32_t ph = 0;
        int64_t amp = (int64_t)n->amp * gain[k] / 1000;
        for (int i = 0; i < len; i++) {
            int32_t sv = sine_tab[ph >> 22];
            int64_t e = env;
            if (i < attack) e = e * i / attack;
            fx_acc[start + i] += (int32_t)((sv * amp >> 15) * e >> 24);
            ph += step;
            env = (uint32_t)(((uint64_t)env * keep) >> 24);
        }
    }
}

static void render(const note_t *notes, int n) {
    int end = 0;
    for (int i = 0; i < FX_MAX; i++) fx_acc[i] = 0;
    for (int i = 0; i < n; i++) {
        bell(&notes[i]);
        int e = (notes[i].at_ms + notes[i].tau_ms * 6) * (AUDIO_RATE / 1000);
        if (e > end) end = e;
    }
    if (end > FX_MAX) end = FX_MAX;
    for (int i = 0; i < end; i++) {
        int32_t v = fx_acc[i];
        fx_pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    fx_len = end;
}

/* Add the sound into the ring over the stretch [a, b) of the stream. */
static void fx_mix(uint64_t a, uint64_t b) {
    if (!fx_len) return;
    uint64_t end = fx_abs0 + (uint64_t)fx_len * 4;
    if (a < fx_done) a = fx_done;
    if (b > end) b = end;
    if (b <= a) return;
    int vol = volume;
    for (uint64_t pos = a; pos < b; pos += 4) {
        int k = (int)((pos - fx_abs0) / 4);
        int add = fx_pcm[k] * vol / 100;
        int o = (int)(pos % BUF_BYTES);
        for (int c = 0; c < 2; c++) {
            int16_t *sp = (int16_t *)(buf + o + c * 2);
            int v = *sp + add;
            *sp = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    fx_done = b;
    if (fx_done >= end) fx_len = 0;
    __asm__ volatile ("sfence" ::: "memory");
}

/* Nothing is writing far enough ahead -- no music, or not enough of it: the
 * sound goes in on its own, a little ahead of the codec, onto silence. */
static void fx_fill(void) {
    if (!fx_len) return;
    play_update();
    if (w_abs < p_abs) w_abs = p_abs;
    uint64_t want = p_abs + FX_AHEAD;
    if (w_abs >= want) return;
    for (uint64_t pos = w_abs; pos < want; pos++) buf[pos % BUF_BYTES] = 0;
    uint64_t from = w_abs;
    w_abs = want;
    wpos = (int)(w_abs % BUF_BYTES);
    fx_mix(from, w_abs);
}

/* The PC speaker plays one note at a time: the tune of each sound. */
static void speaker_tune(const note_t *notes, int n) {
    melody_len = 0;
    for (int i = 0; i < n && melody_len < 8; i++) {
        int until = i + 1 < n ? notes[i + 1].at_ms - notes[i].at_ms : notes[i].tau_ms;
        if (until < 40) until = 40;
        melody[melody_len].hz = notes[i].hz;
        melody[melody_len].ms = (uint16_t)until;
        melody_len++;
    }
    melody_at = 0;
    melody_until = 0;
    audio_tick();
}

void sound_play(int id) {
    /* The notes: when (ms), pitch (Hz), how long it rings (ms), how loud. */
    static const note_t startup[] = {      /* "Течение": a rising Cmaj7, then the octave */
        {   0, 262, 700, 2600 }, {   0, 523, 380, 5200 }, { 120, 659, 380, 5000 },
        { 240, 784, 400, 4800 }, { 360, 988, 420, 4200 }, { 520, 1047, 650, 4600 },
    };
    static const note_t error[]   = { { 0, 440, 160, 7000 }, { 130, 349, 220, 7000 } };
    static const note_t warn[]    = { { 0, 659, 170, 6200 }, { 150, 659, 200, 5600 } };
    static const note_t info[]    = { { 0, 1047, 240, 5600 } };
    static const note_t notify[]  = { { 0, 784, 190, 5600 }, { 95, 1047, 280, 5600 } };
    static const note_t usb_in[]  = { { 0, 523, 140, 6000 }, { 75, 784, 200, 6000 } };
    static const note_t usb_out[] = { { 0, 784, 140, 6000 }, { 75, 523, 200, 6000 } };
    static const struct { const note_t *n; int c; } all[] = {
        { startup, 6 }, { error, 2 }, { warn, 2 }, { info, 1 },
        { notify, 2 }, { usb_in, 2 }, { usb_out, 2 },
    };
    if (id < 0 || id > SND_USB_OUT) return;
    if (!have_hda) { speaker_tune(all[id].n, all[id].c); return; }
    if (!volume) return;

    render(all[id].n, all[id].c);
    play_update();
    fx_abs0 = (p_abs + FX_LEAD) & ~3ULL;
    fx_done = fx_abs0;
    if (w_abs > fx_abs0) fx_mix(fx_abs0, w_abs);   /* over what is queued */
    fx_fill();                                     /* and on from there */
}

void sound_alert(int kind) {
    sound_play(kind == 1 ? SND_WARN : kind == 2 ? SND_INFO : SND_ERROR);
}
