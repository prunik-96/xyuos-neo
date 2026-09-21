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
    uint64_t start = pmm_alloc_contig((bytes + 4095) / 4096);
    return start ? (void *)(uintptr_t)start : NULL;
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

static struct { uint16_t hz; uint16_t ms; } melody[6];
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

void audio_tick(void) {
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
    kprintf("hda: ready, 48 kHz stereo\n");
    return 1;
}

static int play_pos(void) {
    return (int)(r32(sd_off + SD_LPIB) % BUF_BYTES);
}

int audio_queued(void) {
    if (!have_hda) return 0;
    int lp = play_pos();
    int d = wpos - lp;
    if (d < 0) d += BUF_BYTES;
    return d / (2 * AUDIO_CH);
}

int audio_write(const int16_t *frames, int nframes) {
    if (!have_hda || nframes <= 0) return 0;

    int lp = play_pos();
    int used = wpos - lp;
    if (used < 0) used += BUF_BYTES;
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
    wpos = (wpos + want) % BUF_BYTES;

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
    wpos = play_pos();
}

/* --- system sounds --------------------------------------------------------
 * Synthesised on the spot. A square wave through a linear decay is crude, but
 * it is two dozen lines and needs no file on a disk that may be the thing
 * that just failed. */
static void tone_into(int16_t *out, int frames, int hz, int amp_from, int amp_to) {
    int period = AUDIO_RATE / (hz ? hz : 440);
    for (int i = 0; i < frames; i++) {
        int amp = amp_from + (amp_to - amp_from) * i / (frames ? frames : 1);
        int sq = ((i % period) < period / 2) ? amp : -amp;
        out[i * 2 + 0] = (int16_t)sq;
        out[i * 2 + 1] = (int16_t)sq;
    }
}

#define ALERT_FRAMES (AUDIO_RATE / 8)          /* 125 ms per note */
static int16_t alert_buf[ALERT_FRAMES * 2];

void sound_alert(int kind) {
    /* Falling pair for an error, rising for a warning, one soft note for
     * information -- the shape carries the meaning even at low volume. */
    static const struct { int a, b; } notes[3] = {
        { 660, 440 },      /* error   */
        { 520, 700 },      /* warning */
        { 880, 880 },      /* info    */
    };
    if (kind < 0 || kind > 2) kind = 0;

    if (have_hda) {
        tone_into(alert_buf, ALERT_FRAMES, notes[kind].a, 7000, 5000);
        audio_write(alert_buf, ALERT_FRAMES);
        tone_into(alert_buf, ALERT_FRAMES, notes[kind].b, 6000, 0);
        audio_write(alert_buf, ALERT_FRAMES);
        return;
    }

    melody_len = 0;
    melody[melody_len].hz = (uint16_t)notes[kind].a; melody[melody_len].ms = 110; melody_len++;
    melody[melody_len].hz = (uint16_t)notes[kind].b; melody[melody_len].ms = 140; melody_len++;
    melody_at = 0;
    melody_until = 0;
    audio_tick();
}
