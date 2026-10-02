// FAT16, FAT32 and exFAT. See fatfs.h.
//
// Sectors are 512 bytes. Everything is addressed relative to the volume (the
// volume layer adds the partition's start and keeps a small sector cache):
// the FAT and directory sectors go through that cache a sector at a time,
// file data goes around it in runs of whole sectors.

#include "fatfs.h"
#include "vol.h"
#include "../drivers/rtc.h"
#include "../mm/heap.h"
#include "../kernel/kio.h"
#include <stddef.h>

void vol_set_fs(struct vol *v, void *fs);
void vol_free_changed(struct vol *v, int64_t delta);

#define EOC 0xFFFFFFFFu            // end of a chain, whatever the FAT calls it

struct fs {
    int exfat, fat16;
    uint32_t spc;                  // sectors per cluster
    uint32_t csize, cshift;        // bytes per cluster, and its log2
    uint64_t fat_sec;              // the first FAT
    uint32_t fat_len;              // sectors per FAT
    uint32_t nfats;
    uint64_t heap_sec;             // where cluster 2 starts
    uint32_t nclus;                // clusters 2 .. nclus+1
    uint32_t root;                 // root directory's cluster (FAT32, exFAT)
    uint64_t root_sec;             // FAT16's root region
    uint32_t root_ents;
    uint32_t fsinfo;               // FAT32's FSInfo sector, 0 if none
    int fsinfo_done;               // told it its free count is no longer known
    uint32_t bm_first;             // exFAT allocation bitmap
    uint64_t bm_len;
    uint16_t *up;                  // exFAT up-case table: 65536 entries
    uint32_t hint;                 // where to look for a free cluster next
    uint64_t free_clus;
    int free_known;
    char kind[8];
    char label[64];
};

static struct fs *FS(struct vol *v) { return (struct fs *)vol_fs(v); }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

static void bzero(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void bcopy(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    while (n--) *a++ = *b++;
}

static const uint8_t zeros[4096];

// --- clusters ------------------------------------------------------------------

static int valid(struct fs *f, uint32_t c) { return c >= 2 && c < f->nclus + 2; }

static uint64_t clus_sec(struct fs *f, uint32_t c) {
    return f->heap_sec + (uint64_t)(c - 2) * f->spc;
}

// FAT entry of cluster c: the next cluster, EOC, or 0 for a free one.
static uint32_t fat_get(struct vol *v, uint32_t c) {
    struct fs *f = FS(v);
    uint32_t w = f->fat16 ? 2 : 4;
    uint64_t off = (uint64_t)c * w;
    uint8_t *s = vol_sector(v, f->fat_sec + off / 512);
    if (!s) return EOC;
    if (f->fat16) {
        uint32_t x = rd16(s + off % 512);
        return x >= 0xFFF7 ? EOC : x;
    }
    uint32_t x = rd32(s + off % 512);
    if (!f->exfat) x &= 0x0FFFFFFF;
    if (x >= (f->exfat ? 0xFFFFFFF7u : 0x0FFFFFF7u)) return EOC;
    return x;
}

// The volume's free count is about to change: FAT32 keeps a copy of it in
// FSInfo, which from now on is out of date. Saying so ("unknown") is what the
// specification asks; the next system to mount it counts again.
static void fsinfo_unknown(struct vol *v) {
    struct fs *f = FS(v);
    if (f->exfat || f->fat16 || !f->fsinfo || f->fsinfo_done) return;
    f->fsinfo_done = 1;
    uint8_t *s = vol_sector(v, f->fsinfo);
    if (!s || rd32(s) != 0x41615252 || rd32(s + 484) != 0x61417272) return;
    wr32(s + 488, 0xFFFFFFFF);
    wr32(s + 492, 0xFFFFFFFF);
    vol_sector_put(v, f->fsinfo);
}

static int fat_set(struct vol *v, uint32_t c, uint32_t val) {
    struct fs *f = FS(v);
    uint32_t w = f->fat16 ? 2 : 4;
    uint64_t off = (uint64_t)c * w;
    int copies = f->exfat ? 1 : (int)f->nfats;
    for (int k = 0; k < copies; k++) {
        uint64_t sec = f->fat_sec + (uint64_t)k * f->fat_len + off / 512;
        uint8_t *s = vol_sector(v, sec);
        if (!s) return 0;
        if (f->fat16) wr16(s + off % 512, (uint16_t)(val == EOC ? 0xFFFF : val));
        else if (f->exfat) wr32(s + off % 512, val);
        else {
            uint32_t old = rd32(s + off % 512);
            wr32(s + off % 512, (old & 0xF0000000u) | ((val == EOC ? 0x0FFFFFFF : val) & 0x0FFFFFFF));
        }
        if (!vol_sector_put(v, sec)) return 0;
    }
    return 1;
}

// exFAT's allocation bitmap: bit (c - 2) is 1 when cluster c is in use.
static int bm_loc(struct vol *v, uint32_t c, uint64_t *sec, uint32_t *byte) {
    struct fs *f = FS(v);
    uint64_t at = (uint64_t)(c - 2) / 8;
    if (at >= f->bm_len) return 0;
    uint32_t k = (uint32_t)(at >> f->cshift), bc = f->bm_first;
    while (k--) { bc = fat_get(v, bc); if (!valid(f, bc)) return 0; }
    uint64_t in = at & (f->csize - 1);
    *sec = clus_sec(f, bc) + in / 512;
    *byte = (uint32_t)(in % 512);
    return 1;
}

static int is_free(struct vol *v, uint32_t c) {
    struct fs *f = FS(v);
    if (!f->exfat) return fat_get(v, c) == 0;
    uint64_t sec; uint32_t byte;
    if (!bm_loc(v, c, &sec, &byte)) return 0;
    uint8_t *s = vol_sector(v, sec);
    return s && !(s[byte] & (1u << ((c - 2) & 7)));
}

static int bm_set(struct vol *v, uint32_t c, int used) {
    uint64_t sec; uint32_t byte;
    if (!bm_loc(v, c, &sec, &byte)) return 0;
    uint8_t *s = vol_sector(v, sec);
    if (!s) return 0;
    uint8_t bit = (uint8_t)(1u << ((c - 2) & 7));
    s[byte] = used ? (uint8_t)(s[byte] | bit) : (uint8_t)(s[byte] & ~bit);
    return vol_sector_put(v, sec);
}

static void count_changed(struct vol *v, int64_t clusters) {
    struct fs *f = FS(v);
    if (f->free_known) f->free_clus += clusters;
    vol_free_changed(v, clusters * (int64_t)f->csize);
}

// A free cluster, taken: preferably `near` (so a file grows in a row). For a
// FAT it is marked as a chain's end; exFAT marks it in the bitmap only -- a
// file in a row needs no FAT at all. 0 when the volume is full.
static uint32_t alloc_clus(struct vol *v, uint32_t near) {
    struct fs *f = FS(v);
    uint32_t start = valid(f, near) ? near : (valid(f, f->hint) ? f->hint : 2);
    for (uint32_t n = 0; n < f->nclus; n++) {
        uint32_t c = 2 + (start - 2 + n) % f->nclus;
        if (!is_free(v, c)) continue;
        fsinfo_unknown(v);
        if (!(f->exfat ? bm_set(v, c, 1) : fat_set(v, c, EOC))) return 0;
        f->hint = c + 1;
        count_changed(v, -1);
        return c;
    }
    return 0;
}

static void free_clus(struct vol *v, uint32_t c) {
    struct fs *f = FS(v);
    fsinfo_unknown(v);
    if (f->exfat) bm_set(v, c, 0); else fat_set(v, c, 0);
    count_changed(v, 1);
}

// Give back a chain of clusters: `n` in a row from `first` when `contig`,
// otherwise as the FAT links them.
static void free_chain(struct vol *v, uint32_t first, int contig, uint32_t n) {
    struct fs *f = FS(v);
    if (contig) {
        for (uint32_t k = 0; k < n && valid(f, first + k); k++) free_clus(v, first + k);
        return;
    }
    for (uint32_t c = first, guard = 0; valid(f, c) && guard <= f->nclus; guard++) {
        uint32_t next = fat_get(v, c);
        free_clus(v, c);
        c = next;
    }
}

static int zero_clus(struct vol *v, uint32_t c) {
    struct fs *f = FS(v);
    uint64_t s = clus_sec(f, c);
    for (uint32_t k = 0; k < f->spc; k += 8) {
        uint32_t n = f->spc - k < 8 ? f->spc - k : 8;
        if (!vol_write(v, s + k, n, zeros)) return 0;
    }
    return 1;
}

// The k-th cluster of a chain from `first`, through a cursor that remembers
// where the last walk ended (walks mostly go forward).
struct walk { uint32_t first, k, c; };
static uint32_t clus_at(struct vol *v, uint32_t first, int contig, uint32_t k, struct walk *w) {
    struct fs *f = FS(v);
    if (!valid(f, first)) return 0;
    if (contig) return valid(f, first + k) ? first + k : 0;
    uint32_t c = first, i = 0;
    if (w && w->first == first && w->c && w->k <= k) { c = w->c; i = w->k; }
    while (i < k) {
        c = fat_get(v, c);
        if (!valid(f, c)) return 0;
        i++;
    }
    if (w) { w->first = first; w->k = k; w->c = c; }
    return c;
}

// --- time ----------------------------------------------------------------------

// Now, as FAT and exFAT pack it: date << 16 | time, two-second steps.
static uint32_t dos_now(void) {
    struct rtc_time t;
    rtc_read(&t);
    if (t.year < 1980) t.year = 1980;
    uint32_t date = ((uint32_t)(t.year - 1980) << 9) | ((uint32_t)t.mon << 5) | (uint32_t)t.day;
    uint32_t time = ((uint32_t)t.hour << 11) | ((uint32_t)t.min << 5) | (uint32_t)(t.sec / 2);
    return date << 16 | time;
}

static int64_t dos_unix(uint32_t d) {
    if (!(d >> 16)) return 0;
    struct rtc_time t;
    t.year = 1980 + (int)((d >> 25) & 0x7F);
    t.mon = (int)((d >> 21) & 0xF);
    t.day = (int)((d >> 16) & 0x1F);
    t.hour = (int)((d >> 11) & 0x1F);
    t.min = (int)((d >> 5) & 0x3F);
    t.sec = (int)(d & 0x1F) * 2;
    if (t.mon < 1 || t.mon > 12 || t.day < 1) return 0;
    return rtc_to_unix(&t);
}

// --- names ---------------------------------------------------------------------

#define NAME_MAX16 255

// Upper case, for matching names. exFAT carries its own table; for FAT the
// letters Windows would fold: Latin, Latin-1, Cyrillic.
static uint16_t upc(struct fs *f, uint16_t c) {
    if (f && f->up) return f->up[c];
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (uint16_t)(c - 32);
    if (c >= 0x430 && c <= 0x44F) return (uint16_t)(c - 32);
    if (c >= 0x450 && c <= 0x45F) return (uint16_t)(c - 80);
    return c;
}

static int name_eq(struct fs *f, const uint16_t *a, int na, const uint16_t *b, int nb) {
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) if (upc(f, a[i]) != upc(f, b[i])) return 0;
    return 1;
}

// UTF-8 (n bytes) to UTF-16. -1 if it is not valid or too long.
static int to16(const char *s, int n, uint16_t *out) {
    int k = 0;
    for (int i = 0; i < n; ) {
        uint32_t c = (uint8_t)s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) return -1;
        if (len == 2) c &= 0x1F; else if (len == 3) c &= 0x0F; else if (len == 4) c &= 0x07;
        for (int j = 1; j < len; j++) {
            if (((uint8_t)s[i + j] >> 6) != 2) return -1;
            c = (c << 6) | ((uint8_t)s[i + j] & 0x3F);
        }
        i += len;
        if (c >= 0x10000) {
            if (k + 2 > NAME_MAX16) return -1;
            c -= 0x10000;
            out[k++] = (uint16_t)(0xD800 | (c >> 10));
            out[k++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            if (k + 1 > NAME_MAX16) return -1;
            out[k++] = (uint16_t)c;
        }
    }
    return k;
}

static int to8(const uint16_t *s, int n, char *out, int max) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) { if (k + 1 >= max) break; out[k++] = (char)c; }
        else if (c < 0x800) {
            if (k + 2 >= max) break;
            out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            if (k + 3 >= max) break;
            out[k++] = (char)(0xE0 | (c >> 12)); out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (k + 4 >= max) break;
            out[k++] = (char)(0xF0 | (c >> 18)); out[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[k] = 0;
    return k;
}

// A name a new file may have: what Windows allows too.
static int name_ok(const uint16_t *n, int len) {
    if (len < 1 || len > NAME_MAX16) return 0;
    if ((len == 1 && n[0] == '.') || (len == 2 && n[0] == '.' && n[1] == '.')) return 0;
    if (n[len - 1] == ' ' || n[len - 1] == '.') return 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = n[i];
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|') return 0;
    }
    return 1;
}

// --- directories -----------------------------------------------------------------

static struct walk dwalk;            // directories are walked one at a time

static void root_dir(struct vol *v, struct fdir *d) {
    struct fs *f = FS(v);
    bzero(d, sizeof *d);
    d->root = 1;
    if (f->fat16) d->fixed = 1; else d->first = f->root;
}

// The sector and byte where entry i of directory d is. 0 past its end.
static int dent_loc(struct vol *v, const struct fdir *d, uint32_t i, uint64_t *sec, uint32_t *off) {
    struct fs *f = FS(v);
    uint64_t byte = (uint64_t)i * 32;
    if (d->fixed) {
        if (i >= f->root_ents) return 0;
        *sec = f->root_sec + byte / 512;
        *off = (uint32_t)(byte % 512);
        return 1;
    }
    if (d->size && byte >= d->size) return 0;
    uint32_t c = clus_at(v, d->first, d->contig, (uint32_t)(byte >> f->cshift), &dwalk);
    if (!c) return 0;
    uint64_t in = byte & (f->csize - 1);
    *sec = clus_sec(f, c) + in / 512;
    *off = (uint32_t)(in % 512);
    return 1;
}

static int dent_read(struct vol *v, const struct fdir *d, uint32_t i, uint8_t e[32]) {
    uint64_t sec; uint32_t off;
    if (!dent_loc(v, d, i, &sec, &off)) return 0;
    uint8_t *s = vol_sector(v, sec);
    if (!s) return 0;
    bcopy(e, s + off, 32);
    return 1;
}

static int dent_write(struct vol *v, const struct fdir *d, uint32_t i, const uint8_t e[32]) {
    uint64_t sec; uint32_t off;
    if (!dent_loc(v, d, i, &sec, &off)) return 0;
    uint8_t *s = vol_sector(v, sec);
    if (!s) return 0;
    bcopy(s + off, e, 32);
    return vol_sector_put(v, sec);
}

// One entry found in a directory, whatever its format.
struct found {
    uint16_t name[NAME_MAX16 + 1];
    int nlen;
    int ent, nents;                  // its entries: the first, and how many
    uint32_t first;
    uint64_t size, vdl;
    int contig, is_dir;
    uint8_t attr;
    uint32_t mdos;                   // modified: date << 16 | time
};

static uint8_t lfn_sum(const uint8_t *n11) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + n11[i]);
    return s;
}

static const uint8_t lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

// The next entry of a FAT directory from *pos; 1, or 0 at its end.
static int fat_next(struct vol *v, const struct fdir *d, uint32_t *pos, struct found *o) {
    static uint16_t lfn[20 * 13 + 1];
    int lfn_ok = 0, lfn_n = 0, lfn_start = 0;
    uint8_t sum = 0;
    uint8_t e[32];
    for (uint32_t i = *pos;; i++) {
        if (!dent_read(v, d, i, e) || e[0] == 0) { *pos = i; return 0; }
        if (e[0] == 0xE5) { lfn_ok = 0; continue; }
        if (e[11] == 0x0F) {                          // a piece of a long name
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) {
                lfn_ok = seq >= 1 && seq <= 20;
                lfn_n = seq; lfn_start = (int)i; sum = e[13];
                for (int k = 0; k < 20 * 13 + 1; k++) lfn[k] = 0xFFFF;
            } else if (!lfn_ok || e[13] != sum || seq != lfn_n - ((int)i - lfn_start)) {
                lfn_ok = 0;
            }
            if (lfn_ok && seq >= 1)
                for (int k = 0; k < 13; k++) lfn[(seq - 1) * 13 + k] = rd16(e + lfn_at[k]);
            continue;
        }
        if (e[11] & 0x08) { lfn_ok = 0; continue; }  // the volume's label
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { lfn_ok = 0; continue; }

        if (lfn_ok && (int)i - lfn_start == lfn_n && lfn_sum(e) == sum) {
            int n = 0;
            while (n < lfn_n * 13 && lfn[n] != 0 && lfn[n] != 0xFFFF) n++;
            for (int k = 0; k < n; k++) o->name[k] = lfn[k];
            o->nlen = n;
            o->ent = lfn_start;
            o->nents = lfn_n + 1;
        } else {
            // 8.3, lower-cased where Windows NT marked it so.
            int n = 0;
            int lb = e[12] & 0x08, le = e[12] & 0x10;
            for (int k = 0; k < 8 && e[k] != ' '; k++) {
                uint16_t c = (k == 0 && e[0] == 0x05) ? 0xE5 : e[k];
                if (c >= 0x80) c = '_';
                if (lb && c >= 'A' && c <= 'Z') c += 32;
                o->name[n++] = c;
            }
            if (e[8] != ' ') {
                o->name[n++] = '.';
                for (int k = 8; k < 11 && e[k] != ' '; k++) {
                    uint16_t c = e[k] >= 0x80 ? '_' : e[k];
                    if (le && c >= 'A' && c <= 'Z') c += 32;
                    o->name[n++] = c;
                }
            }
            o->nlen = n;
            o->ent = (int)i;
            o->nents = 1;
        }
        o->attr = e[11];
        o->is_dir = (e[11] & 0x10) != 0;
        o->first = ((FS(v)->fat16 ? 0 : (uint32_t)rd16(e + 20)) << 16) | rd16(e + 26);
        o->size = o->is_dir ? 0 : rd32(e + 28);
        o->vdl = o->size;
        o->contig = 0;
        o->mdos = (uint32_t)rd16(e + 24) << 16 | rd16(e + 22);
        *pos = i + 1;
        return 1;
    }
}

// The next entry of an exFAT directory: a file entry, its stream extension
// and its name entries.
static int exfat_next(struct vol *v, const struct fdir *d, uint32_t *pos, struct found *o) {
    uint8_t e[32], s[32], n[32];
    for (uint32_t i = *pos;; i++) {
        if (!dent_read(v, d, i, e) || e[0] == 0) { *pos = i; return 0; }
        if (e[0] != 0x85) continue;
        int sc = e[1];
        if (sc < 2 || sc > 18) continue;
        if (!dent_read(v, d, i + 1, s) || s[0] != 0xC0) continue;
        int nl = s[3], k = 0;
        for (int j = 2; j <= sc && k < nl; j++) {
            if (!dent_read(v, d, i + (uint32_t)j, n) || n[0] != 0xC1) break;
            for (int c = 0; c < 15 && k < nl; c++) o->name[k++] = rd16(n + 2 + 2 * c);
        }
        if (k != nl) { i += (uint32_t)sc; continue; }
        o->nlen = nl;
        o->ent = (int)i;
        o->nents = sc + 1;
        o->attr = (uint8_t)rd16(e + 4);
        o->is_dir = (o->attr & 0x10) != 0;
        o->contig = (s[1] & 2) != 0;
        o->first = rd32(s + 20);
        o->size = rd64(s + 24);
        o->vdl = rd64(s + 8);
        o->mdos = rd32(e + 12);
        *pos = i + (uint32_t)sc + 1;
        return 1;
    }
}

static int dir_next(struct vol *v, const struct fdir *d, uint32_t *pos, struct found *o) {
    return FS(v)->exfat ? exfat_next(v, d, pos, o) : fat_next(v, d, pos, o);
}

static int dir_find(struct vol *v, const struct fdir *d, const uint16_t *name, int nlen,
                    struct found *o) {
    uint32_t pos = 0;
    while (dir_next(v, d, &pos, o))
        if (name_eq(FS(v), o->name, o->nlen, name, nlen)) return 1;
    return 0;
}

// The directory a found entry is, with where its own entry is.
static void as_dir(const struct fdir *in, const struct found *o, struct fdir *d) {
    bzero(d, sizeof *d);
    d->first = o->first;
    d->contig = o->contig;
    d->size = o->size;            // exFAT; FAT keeps 0 for directories
    d->up_first = in->first;
    d->up_contig = in->contig;
    d->up_fixed = in->fixed;
    d->up_size = in->size;
    d->up_ent = o->ent;
}

static void up_dir(const struct fdir *d, struct fdir *up) {
    bzero(up, sizeof *up);
    up->first = d->up_first;
    up->contig = d->up_contig;
    up->fixed = d->up_fixed;
    up->size = d->up_size;
}

// Walk `path` down to the directory its last part is in. That part, as
// UTF-16, into leaf (nleaf 0 for "/"). 0 if a directory on the way is not
// there.
static int walk_path(struct vol *v, const char *path, struct fdir *d, uint16_t *leaf, int *nleaf) {
    static struct found o;
    root_dir(v, d);
    *nleaf = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *e = p;
        while (*e && *e != '/') e++;
        int n = to16(p, (int)(e - p), leaf);
        if (n <= 0) return 0;
        const char *q = e;
        while (*q == '/') q++;
        if (!*q) { *nleaf = n; return 1; }          // the last part
        if (!dir_find(v, d, leaf, n, &o) || !o.is_dir) return 0;
        struct fdir in = *d;
        as_dir(&in, &o, d);
        p = q;
    }
    return 1;
}

// --- exFAT entry sets --------------------------------------------------------------

static uint16_t set_sum(const uint8_t *set, int nents) {
    uint16_t s = 0;
    for (int i = 0; i < nents * 32; i++) {
        if (i == 2 || i == 3) continue;
        s = (uint16_t)(((s & 1) ? 0x8000 : 0) + (s >> 1) + set[i]);
    }
    return s;
}

static uint16_t name_hash(struct fs *f, const uint16_t *n, int len) {
    uint16_t h = 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = upc(f, n[i]);
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
    }
    return h;
}

static uint8_t setbuf[19 * 32];

static int set_read(struct vol *v, const struct fdir *d, int ent, int n) {
    for (int k = 0; k < n; k++) if (!dent_read(v, d, (uint32_t)(ent + k), setbuf + 32 * k)) return 0;
    return 1;
}
static int set_write(struct vol *v, const struct fdir *d, int ent, int n) {
    wr16(setbuf + 2, set_sum(setbuf, n));
    for (int k = 0; k < n; k++) if (!dent_write(v, d, (uint32_t)(ent + k), setbuf + 32 * k)) return 0;
    return 1;
}

// A directory's own stream entry, after it grew.
static int exfat_dir_resized(struct vol *v, const struct fdir *d) {
    if (d->root) return 1;
    struct fdir up;
    up_dir(d, &up);
    if (!dent_read(v, &up, (uint32_t)d->up_ent, setbuf) || setbuf[0] != 0x85) return 0;
    int n = setbuf[1] + 1;
    if (n < 3 || n > 19 || !set_read(v, &up, d->up_ent, n)) return 0;
    uint8_t *s = setbuf + 32;
    s[1] = (uint8_t)((s[1] & ~2) | (d->contig ? 2 : 0) | 1);
    wr32(s + 20, d->first);
    wr64(s + 8, d->size);
    wr64(s + 24, d->size);
    return set_write(v, &up, d->up_ent, n);
}

// Clusters that were in a row get a FAT chain: the next one is not free.
static int make_chain(struct vol *v, uint32_t first, uint32_t n) {
    for (uint32_t k = 0; k + 1 < n; k++) if (!fat_set(v, first + k, first + k + 1)) return 0;
    return n ? fat_set(v, first + n - 1, EOC) : 1;
}

// One more cluster for a directory, zeroed: its entries are all free.
static int dir_grow(struct vol *v, struct fdir *d) {
    struct fs *f = FS(v);
    if (d->fixed) return 0;
    // Its last cluster, and how many it has.
    uint32_t n = 0, last = 0;
    if (d->contig) {
        n = (uint32_t)((d->size + f->csize - 1) >> f->cshift);
        last = d->first + n - 1;
    } else {
        for (uint32_t c = d->first; valid(f, c) && n <= f->nclus; c = fat_get(v, c)) { last = c; n++; }
    }
    uint32_t c = alloc_clus(v, last + 1);
    if (!c) return 0;
    if (!zero_clus(v, c)) return 0;
    if (f->exfat && d->contig && c != last + 1) {
        if (!make_chain(v, d->first, n)) return 0;
        d->contig = 0;
    }
    if (!(f->exfat && d->contig)) {
        if (!fat_set(v, c, EOC) || !fat_set(v, last, c)) return 0;
    }
    dwalk.first = 0;
    if (f->exfat && !d->root) {
        d->size = (uint64_t)(n + 1) << f->cshift;
        return exfat_dir_resized(v, d);
    }
    return 1;
}

// Room for `need` entries in a row: the index of the first.
static int dir_room(struct vol *v, struct fdir *d, int need) {
    struct fs *f = FS(v);
    uint8_t e[32];
    uint32_t run = 0, start = 0;
    for (uint32_t i = 0;; i++) {
        if (!dent_read(v, d, i, e)) {
            if (!dir_grow(v, d)) return -1;
            if (!dent_read(v, d, i, e)) return -1;
        }
        int free_e = f->exfat ? e[0] < 0x80 : (e[0] == 0 || e[0] == 0xE5);
        if (!free_e) { run = 0; continue; }
        if (!run) start = i;
        if (++run == (uint32_t)need) return (int)start;
    }
}

// --- FAT short names -----------------------------------------------------------------

static int short_char(uint16_t c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' || c == '@' || c == '~' ||
           c == '`' || c == '!' || c == '(' || c == ')' || c == '{' || c == '}' || c == '^' ||
           c == '#' || c == '&';
}

// The 8.3 form of a name that is exactly one already (upper case, short).
static int exact_short(const uint16_t *n, int len, uint8_t out[11]) {
    int dot = -1;
    for (int i = 0; i < len; i++) if (n[i] == '.') { if (dot >= 0) return 0; dot = i; }
    int bl = dot < 0 ? len : dot, el = dot < 0 ? 0 : len - dot - 1;
    if (bl < 1 || bl > 8 || el > 3 || (dot >= 0 && el == 0)) return 0;
    for (int i = 0; i < 11; i++) out[i] = ' ';
    for (int i = 0; i < bl; i++) { if (!short_char(n[i])) return 0; out[i] = (uint8_t)n[i]; }
    for (int i = 0; i < el; i++) { if (!short_char(n[dot + 1 + i])) return 0; out[8 + i] = (uint8_t)n[dot + 1 + i]; }
    return 1;
}

static int short_taken(struct vol *v, const struct fdir *d, const uint8_t n11[11]) {
    uint8_t e[32];
    for (uint32_t i = 0; dent_read(v, d, i, e) && e[0]; i++) {
        if (e[0] == 0xE5 || e[11] == 0x0F) continue;
        int same = 1;
        for (int k = 0; k < 11; k++) if (e[k] != n11[k]) { same = 0; break; }
        if (same) return 1;
    }
    return 0;
}

// A short name for a long one, unique in the directory: BASIS~N.EXT.
static int make_short(struct vol *v, const struct fdir *d, const uint16_t *n, int len, uint8_t out[11]) {
    uint8_t base[8], ext[3];
    int bl = 0, el = 0, dot = -1;
    for (int i = len - 1; i >= 0; i--) if (n[i] == '.') { dot = i; break; }
    for (int i = 0; i < (dot < 0 ? len : dot) && bl < 8; i++) {
        uint16_t c = upc(NULL, n[i]);
        if (c == ' ' || c == '.') continue;
        base[bl++] = (uint8_t)(short_char(c) ? c : '_');
    }
    if (dot >= 0)
        for (int i = dot + 1; i < len && el < 3; i++) {
            uint16_t c = upc(NULL, n[i]);
            if (c == ' ') continue;
            ext[el++] = (uint8_t)(short_char(c) ? c : '_');
        }
    if (!bl) base[bl++] = '_';
    for (uint32_t tail = 1; tail < 1000000; tail++) {
        char t[8];
        int tl = 0;
        for (uint32_t x = tail; x; x /= 10) t[tl++] = (char)('0' + x % 10);
        int keep = bl;
        if (keep > 7 - tl) keep = 7 - tl;
        for (int i = 0; i < 11; i++) out[i] = ' ';
        for (int i = 0; i < keep; i++) out[i] = base[i];
        out[keep] = '~';
        for (int i = 0; i < tl; i++) out[keep + 1 + i] = (uint8_t)t[tl - 1 - i];
        for (int i = 0; i < el; i++) out[8 + i] = ext[i];
        if (!short_taken(v, d, out)) return 1;
    }
    return 0;
}

// --- making entries -----------------------------------------------------------------

// What a new entry carries. Times are FAT-packed; `times` (13 bytes, FAT short
// entry bytes 13..25, or exFAT file entry bytes 8..20) copies an old entry's.
struct newent {
    uint32_t first;
    uint64_t size;
    int contig, is_dir;
    uint8_t attr;
    const uint8_t *times;
};

// Write the entries for `name` in d. Returns the index of the first, -1 on failure.
static int put_entry(struct vol *v, struct fdir *d, const uint16_t *name, int nlen,
                     const struct newent *ne, int *nents) {
    struct fs *f = FS(v);
    uint32_t now = dos_now();
    if (f->exfat) {
        int names = (nlen + 14) / 15, n = 2 + names;
        int at = dir_room(v, d, n);
        if (at < 0) return -1;
        bzero(setbuf, (size_t)n * 32);
        uint8_t *fe = setbuf, *se = setbuf + 32;
        fe[0] = 0x85;
        fe[1] = (uint8_t)(n - 1);
        wr16(fe + 4, ne->attr);
        if (ne->times) bcopy(fe + 8, ne->times, 13);
        else { wr32(fe + 8, now); wr32(fe + 12, now); wr32(fe + 16, now); }
        se[0] = 0xC0;
        se[1] = (uint8_t)(1 | (ne->contig && ne->first ? 2 : 0));
        se[3] = (uint8_t)nlen;
        wr16(se + 4, name_hash(f, name, nlen));
        wr64(se + 8, ne->size);
        wr32(se + 20, ne->first);
        wr64(se + 24, ne->size);
        for (int k = 0; k < names; k++) {
            uint8_t *ce = setbuf + 64 + 32 * k;
            ce[0] = 0xC1;
            for (int c = 0; c < 15; c++) {
                int i = k * 15 + c;
                wr16(ce + 2 + 2 * c, i < nlen ? name[i] : 0);
            }
        }
        if (!set_write(v, d, at, n)) return -1;
        *nents = n;
        return at;
    }

    uint8_t n11[11];
    int lfn = 0;
    if (!exact_short(name, nlen, n11) || short_taken(v, d, n11)) {
        if (!make_short(v, d, name, nlen, n11)) return -1;
        lfn = (nlen + 12) / 13;
    }
    int at = dir_room(v, d, lfn + 1);
    if (at < 0) return -1;
    uint8_t sum = lfn_sum(n11), e[32];
    for (int k = lfn; k >= 1; k--) {
        bzero(e, 32);
        e[0] = (uint8_t)(k | (k == lfn ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (int c = 0; c < 13; c++) {
            int i = (k - 1) * 13 + c;
            wr16(e + lfn_at[c], i < nlen ? name[i] : i == nlen ? 0 : 0xFFFF);
        }
        if (!dent_write(v, d, (uint32_t)(at + lfn - k), e)) return -1;
    }
    bzero(e, 32);
    bcopy(e, n11, 11);
    e[11] = ne->attr;
    if (ne->times) bcopy(e + 13, ne->times, 13);
    else {
        wr16(e + 14, (uint16_t)now); wr16(e + 16, (uint16_t)(now >> 16));   // created
        wr16(e + 18, (uint16_t)(now >> 16));                                // accessed
        wr16(e + 22, (uint16_t)now); wr16(e + 24, (uint16_t)(now >> 16));   // written
    }
    wr16(e + 20, (uint16_t)(ne->first >> 16));
    wr16(e + 26, (uint16_t)ne->first);
    wr32(e + 28, ne->is_dir ? 0 : (uint32_t)ne->size);
    if (!dent_write(v, d, (uint32_t)(at + lfn), e)) return -1;
    *nents = lfn + 1;
    return at;
}

static int drop_entries(struct vol *v, const struct fdir *d, int ent, int n) {
    uint8_t e[32];
    for (int k = 0; k < n; k++) {
        if (!dent_read(v, d, (uint32_t)(ent + k), e)) return 0;
        if (FS(v)->exfat) e[0] &= 0x7F; else e[0] = 0xE5;
        if (!dent_write(v, d, (uint32_t)(ent + k), e)) return 0;
    }
    return 1;
}

static void fill_file(struct ffile *f, const struct fdir *in, const struct found *o) {
    bzero(f, sizeof *f);
    f->in = *in;
    f->ent = o->ent;
    f->nents = o->nents;
    f->first = o->first;
    f->size = o->size;
    f->contig = o->contig;
    f->is_dir = o->is_dir;
    f->attr = o->attr;
    f->vdl = o->vdl;
    f->mdos = o->mdos;
}

// --- the operations -------------------------------------------------------------------

int fatfs_open(struct vol *v, const char *path, struct ffile *f) {
    static uint16_t leaf[NAME_MAX16 + 1];
    static struct found o;
    struct fdir d;
    int n;
    if (!walk_path(v, path, &d, leaf, &n)) return 0;
    if (!n) {                                         // the root
        bzero(f, sizeof *f);
        f->is_dir = 1;
        f->first = d.first;
        f->in = d;
        f->ent = -1;
        return 1;
    }
    if (!dir_find(v, &d, leaf, n, &o)) return 0;
    fill_file(f, &d, &o);
    return 1;
}

int fatfs_create(struct vol *v, const char *path, int is_dir, struct ffile *f) {
    static uint16_t leaf[NAME_MAX16 + 1];
    static struct found o;
    struct fs *fs = FS(v);
    struct fdir d;
    int n;
    if (!walk_path(v, path, &d, leaf, &n) || !n || !name_ok(leaf, n)) return 0;
    if (dir_find(v, &d, leaf, n, &o)) return 0;

    struct newent ne;
    bzero(&ne, sizeof ne);
    ne.is_dir = is_dir;
    ne.attr = is_dir ? 0x10 : 0x20;
    if (is_dir) {
        uint32_t c = alloc_clus(v, 0);
        if (!c || !zero_clus(v, c)) return 0;
        ne.first = c;
        if (fs->exfat) { ne.contig = 1; ne.size = fs->csize; }
        else {
            // "." and "..": itself, and the directory above (0 for the root).
            uint8_t e[32];
            struct fdir self;
            bzero(&self, sizeof self);
            self.first = c;
            uint32_t now = dos_now();
            for (int k = 0; k < 2; k++) {
                bzero(e, 32);
                for (int i = 0; i < 11; i++) e[i] = ' ';
                e[0] = '.';
                if (k) e[1] = '.';
                e[11] = 0x10;
                uint32_t to = k ? (d.root ? 0 : d.first) : c;
                wr16(e + 20, (uint16_t)(to >> 16));
                wr16(e + 26, (uint16_t)to);
                wr16(e + 22, (uint16_t)now); wr16(e + 24, (uint16_t)(now >> 16));
                if (!dent_write(v, &self, (uint32_t)k, e)) return 0;
            }
        }
    }
    int nents;
    int at = put_entry(v, &d, leaf, n, &ne, &nents);
    if (at < 0) {
        if (ne.first) free_chain(v, ne.first, 1, 1);
        return 0;
    }
    bzero(f, sizeof *f);
    f->in = d;
    f->ent = at;
    f->nents = nents;
    f->first = ne.first;
    f->size = ne.size;
    f->vdl = ne.size;
    f->contig = ne.contig;
    f->is_dir = is_dir;
    f->attr = ne.attr;
    f->mdos = dos_now();
    return 1;
}

// The k-th cluster of an open file, through its own cursor.
static uint32_t file_clus(struct vol *v, struct ffile *f, uint32_t k) {
    struct walk w = { f->first, (uint32_t)(f->cpos >> FS(v)->cshift), f->cclus };
    uint32_t c = clus_at(v, f->first, f->contig, k, &w);
    if (c && !f->contig) { f->cpos = (uint64_t)w.k << FS(v)->cshift; f->cclus = w.c; }
    return c;
}

// How many clusters the file has, and its last: counted once, then kept.
static uint32_t file_nclus(struct vol *v, struct ffile *f, uint32_t *last) {
    struct fs *fs = FS(v);
    *last = 0;
    if (!valid(fs, f->first)) return 0;
    if (f->ncl) { *last = f->lastc; return f->ncl; }
    uint32_t n = 0;
    if (f->contig) {
        n = (uint32_t)((f->size + fs->csize - 1) >> fs->cshift);
        *last = n ? f->first + n - 1 : 0;
    } else {
        for (uint32_t c = f->first; valid(fs, c) && n <= fs->nclus; c = fat_get(v, c)) { *last = c; n++; }
    }
    f->ncl = n;
    f->lastc = *last;
    return n;
}

// Clusters enough for `bytes`.
static int file_reserve(struct vol *v, struct ffile *f, uint64_t bytes) {
    struct fs *fs = FS(v);
    uint32_t need = (uint32_t)((bytes + fs->csize - 1) >> fs->cshift), last;
    uint32_t have = file_nclus(v, f, &last);
    while (have < need) {
        uint32_t c = alloc_clus(v, last ? last + 1 : 0);
        if (!c) return 0;
        if (!have) {
            f->first = c;
            f->contig = fs->exfat;               // exFAT: a run needs no FAT
        } else if (fs->exfat && f->contig && c != last + 1) {
            if (!make_chain(v, f->first, have)) return 0;
            f->contig = 0;
        }
        if (!(fs->exfat && f->contig)) {
            if (!fat_set(v, c, EOC)) return 0;
            if (have && !fat_set(v, last, c)) return 0;
        }
        last = c;
        have++;
        f->ncl = have;
        f->lastc = last;
        f->dirty = 1;
    }
    return 1;
}

long fatfs_pread(struct vol *v, struct ffile *f, uint64_t off, void *buf, uint32_t len) {
    struct fs *fs = FS(v);
    if (f->is_dir) return -1;
    if (off >= f->size) return 0;
    if (len > f->size - off) len = (uint32_t)(f->size - off);
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t want = len - done;
        if (pos >= f->vdl) {                            // past what was ever written: zeros
            bzero(out + done, want);
            done += want;
            break;
        }
        if (pos + want > f->vdl) want = (uint32_t)(f->vdl - pos);
        uint32_t c = file_clus(v, f, (uint32_t)(pos >> fs->cshift));
        if (!c) return done ? (long)done : -1;
        uint64_t in = pos & (fs->csize - 1);
        uint64_t sec = clus_sec(fs, c) + in / 512;
        uint32_t soff = (uint32_t)(in % 512);
        if (soff || want < 512) {
            uint8_t *s = vol_sector(v, sec);
            if (!s) return done ? (long)done : -1;
            uint32_t k = 512 - soff < want ? 512 - soff : want;
            bcopy(out + done, s + soff, k);
            done += k;
            continue;
        }
        // Whole sectors: to the end of this cluster, and on through the
        // clusters right after it, in one transfer.
        uint64_t run = fs->csize - in;
        uint32_t cc = c;
        while (run < want && run < 65536) {
            uint32_t next = f->contig ? cc + 1 : fat_get(v, cc);
            if (next != cc + 1 || !valid(fs, next)) break;
            cc = next;
            run += fs->csize;
        }
        uint32_t nsec = (uint32_t)((run < want ? run : want) / 512);
        if (nsec > 128) nsec = 128;
        if (!vol_read(v, sec, nsec, out + done)) return done ? (long)done : -1;
        done += nsec * 512;
    }
    return (long)done;
}

long fatfs_pwrite(struct vol *v, struct ffile *f, uint64_t off, const void *buf, uint32_t len) {
    struct fs *fs = FS(v);
    if (f->is_dir) return -1;
    if (!len) return 0;
    if (!fs->exfat && off + len > 0xFFFFFFFFull) return -1;   // FAT's limit: 4 GiB - 1
    // A gap before it -- or what lies past what was ever written -- becomes zeros.
    uint64_t from = f->vdl < f->size ? f->vdl : f->size;
    if (off > from) {
        uint64_t at = from;
        while (at < off) {
            uint32_t k = off - at > sizeof zeros ? (uint32_t)sizeof zeros : (uint32_t)(off - at);
            if (fatfs_pwrite(v, f, at, zeros, k) != (long)k) return -1;
            at += k;
        }
    }
    uint64_t end = off + len;
    if (!file_reserve(v, f, end)) return -1;
    const uint8_t *in8 = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t want = len - done;
        uint32_t c = file_clus(v, f, (uint32_t)(pos >> fs->cshift));
        if (!c) return -1;
        uint64_t in = pos & (fs->csize - 1);
        uint64_t sec = clus_sec(fs, c) + in / 512;
        uint32_t soff = (uint32_t)(in % 512);
        if (soff || want < 512) {
            uint8_t *s = vol_sector(v, sec);
            if (!s) return -1;
            uint32_t k = 512 - soff < want ? 512 - soff : want;
            bcopy(s + soff, in8 + done, k);
            if (!vol_sector_put(v, sec)) return -1;
            done += k;
            continue;
        }
        uint64_t run = fs->csize - in;
        uint32_t cc = c;
        while (run < want && run < 65536) {
            uint32_t next = f->contig ? cc + 1 : fat_get(v, cc);
            if (next != cc + 1 || !valid(fs, next)) break;
            cc = next;
            run += fs->csize;
        }
        uint32_t nsec = (uint32_t)((run < want ? run : want) / 512);
        if (nsec > 128) nsec = 128;
        if (!vol_write(v, sec, nsec, in8 + done)) return -1;
        done += nsec * 512;
    }
    if (end > f->size) f->size = end;
    if (end > f->vdl) f->vdl = end;
    f->mdos = dos_now();
    f->dirty = 1;
    return (long)len;
}

int fatfs_truncate(struct vol *v, struct ffile *f, uint64_t len) {
    struct fs *fs = FS(v);
    if (f->is_dir) return 0;
    if (len > f->size) {
        uint64_t at = f->size;
        while (at < len) {
            uint32_t k = len - at > sizeof zeros ? (uint32_t)sizeof zeros : (uint32_t)(len - at);
            if (fatfs_pwrite(v, f, at, zeros, k) != (long)k) return 0;
            at += k;
        }
        return 1;
    }
    if (len == f->size) return 1;
    uint32_t last;
    uint32_t have = file_nclus(v, f, &last);
    uint32_t keep = (uint32_t)((len + fs->csize - 1) >> fs->cshift);
    if (keep == 0) {
        if (have) free_chain(v, f->first, f->contig, have);
        f->first = 0;
        f->contig = 0;
        f->ncl = f->lastc = 0;
    } else if (keep < have) {
        uint32_t lastkeep = file_clus(v, f, keep - 1);
        if (!lastkeep) return 0;
        if (f->contig) free_chain(v, f->first + keep, 1, have - keep);
        else {
            uint32_t rest = fat_get(v, lastkeep);
            if (!fat_set(v, lastkeep, EOC)) return 0;
            if (valid(fs, rest)) free_chain(v, rest, 0, 0);
        }
        f->ncl = keep;
        f->lastc = lastkeep;
    }
    f->size = len;
    if (f->vdl > len) f->vdl = len;
    f->cpos = 0;
    f->cclus = 0;
    f->mdos = dos_now();
    f->dirty = 1;
    return 1;
}

int fatfs_sync(struct vol *v, struct ffile *f) {
    struct fs *fs = FS(v);
    if (!f->dirty || f->ent < 0) return 1;
    if (fs->exfat) {
        if (!set_read(v, &f->in, f->ent, f->nents) || setbuf[0] != 0x85 || setbuf[32] != 0xC0) return 0;
        uint8_t *fe = setbuf, *se = setbuf + 32;
        wr32(fe + 12, f->mdos);
        fe[21] = 0;
        fe[23] = 0;
        se[1] = (uint8_t)(1 | (f->contig && f->first ? 2 : 0));
        wr32(se + 20, f->first);
        wr64(se + 8, f->vdl);
        wr64(se + 24, f->size);
        if (!set_write(v, &f->in, f->ent, f->nents)) return 0;
    } else {
        uint8_t e[32];
        uint32_t at = (uint32_t)(f->ent + f->nents - 1);
        if (!dent_read(v, &f->in, at, e)) return 0;
        wr16(e + 20, (uint16_t)(f->first >> 16));
        wr16(e + 26, (uint16_t)f->first);
        wr32(e + 28, f->is_dir ? 0 : (uint32_t)f->size);
        wr16(e + 22, (uint16_t)f->mdos);
        wr16(e + 24, (uint16_t)(f->mdos >> 16));
        wr16(e + 18, (uint16_t)(f->mdos >> 16));
        e[11] |= f->is_dir ? 0 : 0x20;                  // archive: it changed
        if (!dent_write(v, &f->in, at, e)) return 0;
    }
    f->dirty = 0;
    return 1;
}

int64_t fatfs_mtime(struct vol *v, struct ffile *f) { (void)v; return dos_unix(f->mdos); }

int fatfs_list(struct vol *v, const char *path, fatfs_list_cb cb, void *ctx) {
    static struct found o;
    static char name8[NAME_MAX16 * 3 + 1];
    struct ffile f;
    if (!fatfs_open(v, path, &f) || !f.is_dir) return 0;
    struct fdir d;
    if (f.ent < 0) d = f.in;                          // the root
    else {
        struct found me;
        bzero(&me, sizeof me);
        me.first = f.first; me.contig = f.contig; me.size = f.size; me.ent = f.ent;
        as_dir(&f.in, &me, &d);
    }
    uint32_t pos = 0;
    while (dir_next(v, &d, &pos, &o)) {
        to8(o.name, o.nlen, name8, sizeof name8);
        cb(name8, o.is_dir, o.size, dos_unix(o.mdos), ctx);
    }
    return 1;
}

static int dir_empty(struct vol *v, const struct fdir *d) {
    static struct found o;
    uint32_t pos = 0;
    return !dir_next(v, d, &pos, &o);
}

int fatfs_unlink(struct vol *v, const char *path) {
    struct ffile f;
    if (!fatfs_open(v, path, &f) || f.ent < 0) return 0;
    if (f.is_dir) {
        struct found me;
        struct fdir d;
        bzero(&me, sizeof me);
        me.first = f.first; me.contig = f.contig; me.size = f.size; me.ent = f.ent;
        as_dir(&f.in, &me, &d);
        if (!dir_empty(v, &d)) return 0;
    }
    if (!drop_entries(v, &f.in, f.ent, f.nents)) return 0;
    uint32_t last;
    uint32_t n = file_nclus(v, &f, &last);
    if (n) free_chain(v, f.first, f.contig, n);
    dwalk.first = 0;
    return 1;
}

// Whether `to` lies inside `from` (a directory cannot move into itself).
static int inside(struct fs *fs, const char *from, const char *to) {
    uint16_t a[NAME_MAX16 + 1], b[NAME_MAX16 + 1];
    while (*from) {
        while (*from == '/') from++;
        while (*to == '/') to++;
        if (!*from) break;
        const char *e1 = from, *e2 = to;
        while (*e1 && *e1 != '/') e1++;
        while (*e2 && *e2 != '/') e2++;
        int na = to16(from, (int)(e1 - from), a), nb = to16(to, (int)(e2 - to), b);
        if (na < 0 || nb < 0 || !name_eq(fs, a, na, b, nb)) return 0;
        from = e1;
        to = e2;
    }
    while (*to == '/') to++;
    return *to != 0;                     // something below it, not itself
}

int fatfs_rename(struct vol *v, const char *from, const char *to) {
    static uint16_t leaf[NAME_MAX16 + 1];
    static struct found o;
    struct fs *fs = FS(v);
    struct ffile s;
    if (!fatfs_open(v, from, &s) || s.ent < 0) return 0;
    struct fdir d;
    int n;
    if (!walk_path(v, to, &d, leaf, &n) || !n || !name_ok(leaf, n)) return 0;
    if (s.is_dir && inside(fs, from, to)) return 0;
    if (dir_find(v, &d, leaf, n, &o)) {
        // Only the same entry, its name's case changing, may be "there" already.
        if (!(o.ent == s.ent && o.first == s.first && d.first == s.in.first)) return 0;
    }
    // The old entry's times and attributes travel with it.
    uint8_t times[13], old[32];
    struct newent ne;
    bzero(&ne, sizeof ne);
    if (fs->exfat) {
        if (!set_read(v, &s.in, s.ent, 1)) return 0;
        bcopy(times, setbuf + 8, 13);
        ne.attr = (uint8_t)rd16(setbuf + 4);
    } else {
        if (!dent_read(v, &s.in, (uint32_t)(s.ent + s.nents - 1), old)) return 0;
        bcopy(times, old + 13, 13);
        ne.attr = old[11];
    }
    ne.first = s.first;
    ne.size = s.is_dir && fs->exfat ? s.size : s.size;
    ne.contig = s.contig;
    ne.is_dir = s.is_dir;
    ne.times = times;
    // The old entries go first: renamed in place, the new ones may need their room.
    if (!drop_entries(v, &s.in, s.ent, s.nents)) return 0;
    int nents;
    if (put_entry(v, &d, leaf, n, &ne, &nents) < 0) {
        // Put it back where it was rather than lose it.
        uint8_t e[32];
        for (int k = 0; k < s.nents; k++) {
            if (!dent_read(v, &s.in, (uint32_t)(s.ent + k), e)) break;
            // exFAT keeps the type with its in-use bit cleared; a FAT long-name
            // piece's first byte is its sequence number, known from its place.
            if (fs->exfat) e[0] |= 0x80;
            else e[0] = (uint8_t)((s.nents - 1 - k) | (k == 0 ? 0x40 : 0));
            dent_write(v, &s.in, (uint32_t)(s.ent + k), e);
        }
        if (!fs->exfat) dent_write(v, &s.in, (uint32_t)(s.ent + s.nents - 1), old);
        return 0;
    }
    // A FAT directory that moved: its ".." now names the new parent.
    if (!fs->exfat && s.is_dir && d.first != s.in.first) {
        struct fdir self;
        bzero(&self, sizeof self);
        self.first = s.first;
        uint8_t e[32];
        if (dent_read(v, &self, 1, e) && e[0] == '.' && e[1] == '.') {
            uint32_t up = d.root ? 0 : d.first;
            wr16(e + 20, (uint16_t)(up >> 16));
            wr16(e + 26, (uint16_t)up);
            dent_write(v, &self, 1, e);
        }
    }
    dwalk.first = 0;
    return 1;
}

// --- mounting ------------------------------------------------------------------------

// Read `len` bytes of a chain from its start (the up-case table).
static int read_chain(struct vol *v, uint32_t first, uint64_t len, uint8_t *out) {
    struct fs *f = FS(v);
    uint64_t done = 0;
    uint32_t c = first;
    while (done < len) {
        if (!valid(f, c)) return 0;
        uint64_t k = len - done < f->csize ? len - done : f->csize;
        uint32_t ns = (uint32_t)((k + 511) / 512);
        static uint8_t tmp[512];
        for (uint32_t s = 0; s < ns; s++) {
            if (!vol_read(v, clus_sec(f, c) + s, 1, tmp)) return 0;
            uint64_t take = k - (uint64_t)s * 512 < 512 ? k - (uint64_t)s * 512 : 512;
            bcopy(out + done + (uint64_t)s * 512, tmp, take);
        }
        done += k;
        c = fat_get(v, c);
    }
    return 1;
}

static int exfat_mount(struct vol *v, struct fs *f, const uint8_t *b) {
    if (b[108] != 9) return 0;                         // 512-byte sectors only
    if (b[109] > 16) return 0;                         // clusters up to 32 MiB
    f->exfat = 1;
    f->spc = 1u << b[109];
    f->fat_sec = rd32(b + 80);
    f->fat_len = rd32(b + 84);
    f->heap_sec = rd32(b + 88);
    f->nclus = rd32(b + 92);
    f->root = rd32(b + 96);
    f->nfats = b[110];
    if (!f->nclus || f->heap_sec + (uint64_t)f->nclus * f->spc > vol_sectors(v)) return 0;
    for (uint32_t s = 0; (1u << s) < f->spc * 512; s++) f->cshift = s + 1;
    f->csize = f->spc * 512;

    // The root's system entries: the allocation bitmap, the up-case table,
    // the label.
    struct fdir root;
    root_dir(v, &root);
    uint8_t e[32];
    uint32_t up_first = 0;
    uint64_t up_len = 0;
    for (uint32_t i = 0; dent_read(v, &root, i, e) && e[0]; i++) {
        if (e[0] == 0x81 && !f->bm_first && !(e[1] & 1)) { f->bm_first = rd32(e + 20); f->bm_len = rd64(e + 24); }
        else if (e[0] == 0x82) { up_first = rd32(e + 20); up_len = rd64(e + 24); }
        else if (e[0] == 0x83) {
            uint16_t l[11];
            int n = e[1] > 11 ? 11 : e[1];
            for (int k = 0; k < n; k++) l[k] = rd16(e + 2 + 2 * k);
            to8(l, n, f->label, sizeof f->label);
        }
    }
    if (!valid(f, f->bm_first) || f->bm_len * 8 < f->nclus) return 0;
    f->up = (uint16_t *)kmalloc(65536 * 2);
    if (!f->up) return 0;
    for (uint32_t i = 0; i < 65536; i++) f->up[i] = (uint16_t)i;
    if (valid(f, up_first) && up_len >= 2 && up_len <= 131072) {
        uint8_t *t = (uint8_t *)kmalloc((size_t)up_len + 512);
        if (t && read_chain(v, up_first, up_len, t)) {
            // Compressed: 0xFFFF n means the next n characters map to themselves.
            uint32_t idx = 0;
            for (uint64_t p = 0; p + 1 < up_len && idx < 65536; p += 2) {
                uint16_t x = rd16(t + p);
                if (x == 0xFFFF && p + 3 < up_len) { idx += rd16(t + p + 2); p += 2; }
                else f->up[idx++] = x;
            }
        }
        if (t) kfree(t);
    }
    for (int i = 0; i < 6; i++) f->kind[i] = "exFAT"[i];
    return 1;
}

static int fat_mount(struct vol *v, struct fs *f, const uint8_t *b) {
    uint32_t spc = b[13], rsv = rd16(b + 14), nfats = b[16], rootents = rd16(b + 17);
    uint32_t tot = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    uint32_t fatsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    if (!spc || !nfats || !fatsz || tot > vol_sectors(v)) return 0;
    uint32_t rootsecs = (rootents * 32 + 511) / 512;
    uint32_t data = rsv + nfats * fatsz + rootsecs;
    if (data >= tot) return 0;
    uint32_t nclus = (tot - data) / spc;
    if (nclus < 4085) return 0;                        // FAT12: floppies, not drives
    f->fat16 = nclus < 65525;
    f->spc = spc;
    f->csize = spc * 512;
    for (uint32_t s = 0; (1u << s) < f->csize; s++) f->cshift = s + 1;
    f->fat_sec = rsv;
    f->fat_len = fatsz;
    f->nfats = nfats;
    f->heap_sec = data;
    f->nclus = nclus;
    f->root_sec = rsv + nfats * fatsz;
    f->root_ents = rootents;
    if (!f->fat16) {
        f->root = rd32(b + 44);
        f->fsinfo = rd16(b + 48);
        if (!valid(f, f->root)) return 0;
    }
    const char *k = f->fat16 ? "FAT16" : "FAT32";
    for (int i = 0; i < 6; i++) f->kind[i] = k[i];

    // The label: the root's volume-label entry, else the boot sector's.
    struct fdir root;
    root_dir(v, &root);
    uint8_t e[32], lab[11];
    int have = 0;
    for (uint32_t i = 0; dent_read(v, &root, i, e) && e[0] && i < 512; i++) {
        if (e[0] != 0xE5 && e[11] != 0x0F && (e[11] & 0x08)) { bcopy(lab, e, 11); have = 1; break; }
    }
    if (!have) { bcopy(lab, b + (f->fat16 ? 43 : 71), 11); have = 1; }
    int n = 11;
    while (n && lab[n - 1] == ' ') n--;
    static const char noname[11] = { 'N', 'O', ' ', 'N', 'A', 'M', 'E', ' ', ' ', ' ', ' ' };
    int is_noname = 1;
    for (int i = 0; i < 11; i++) if (lab[i] != (uint8_t)noname[i]) is_noname = 0;
    if (!is_noname) {
        int m = 0;
        for (int i = 0; i < n && m < 62; i++) f->label[m++] = (char)(lab[i] < 0x80 ? lab[i] : '_');
        f->label[m] = 0;
    }
    return 1;
}

int fatfs_mount(struct vol *v, const uint8_t *boot) {
    struct fs *f = (struct fs *)kmalloc(sizeof *f);
    if (!f) return 0;
    bzero(f, sizeof *f);
    vol_set_fs(v, f);
    f->hint = 2;
    static const char ex[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    int is_ex = 1;
    for (int i = 0; i < 8; i++) if (boot[3 + i] != (uint8_t)ex[i]) is_ex = 0;
    if (is_ex ? exfat_mount(v, f, boot) : fat_mount(v, f, boot)) return 1;
    fatfs_unmount(v);
    return 0;
}

void fatfs_unmount(struct vol *v) {
    struct fs *f = FS(v);
    if (!f) return;
    if (f->up) kfree(f->up);
    kfree(f);
    vol_set_fs(v, NULL);
    dwalk.first = 0;
}

const char *fatfs_kind(struct vol *v) { return FS(v) ? FS(v)->kind : ""; }

int fatfs_label(struct vol *v, char *out, int max) {
    struct fs *f = FS(v);
    int i = 0;
    for (; f && f->label[i] && i < max - 1; i++) out[i] = f->label[i];
    out[i] = 0;
    return i > 0;
}

int fatfs_size(struct vol *v, uint64_t *total) {
    struct fs *f = FS(v);
    if (!f) return 0;
    *total = (uint64_t)f->nclus * f->csize;
    return 1;
}

int fatfs_free(struct vol *v, uint64_t *free_bytes) {
    struct fs *f = FS(v);
    if (!f) return 0;
    if (!f->free_known) {
        // Counted from the FAT (or the bitmap) read in big pieces: a sector at
        // a time, a big drive's FAT is minutes of USB commands.
        static uint8_t buf[65536];
        uint64_t n = 0;
        if (f->exfat) {
            uint64_t bytes = (f->nclus + 7) / 8, done = 0;
            uint32_t c = f->bm_first;
            while (done < bytes && valid(f, c)) {
                uint64_t k = bytes - done < f->csize ? bytes - done : f->csize;
                for (uint64_t at = 0; at < k; at += sizeof buf) {
                    uint32_t take = k - at < sizeof buf ? (uint32_t)(k - at) : (uint32_t)sizeof buf;
                    if (!vol_read(v, clus_sec(f, c) + at / 512, (take + 511) / 512, buf)) return 0;
                    for (uint32_t i = 0; i < take; i++) {
                        uint64_t bit0 = (done + at + i) * 8;
                        for (int b = 0; b < 8; b++)
                            if (bit0 + b < f->nclus && !(buf[i] & (1 << b))) n++;
                    }
                }
                done += k;
                c = fat_get(v, c);
            }
        } else {
            uint32_t w = f->fat16 ? 2 : 4;
            uint64_t entries = (uint64_t)f->nclus + 2;
            for (uint64_t e0 = 0; e0 < entries; e0 += sizeof buf / w) {
                uint64_t k = entries - e0 < sizeof buf / w ? entries - e0 : sizeof buf / w;
                uint64_t sec = f->fat_sec + e0 * w / 512;
                if (!vol_read(v, sec, (uint32_t)((k * w + 511) / 512), buf)) return 0;
                for (uint64_t i = 0; i < k; i++) {
                    uint64_t c = e0 + i;
                    if (c < 2) continue;
                    uint32_t x = w == 2 ? rd16(buf + i * 2) : (rd32(buf + i * 4) & 0x0FFFFFFF);
                    if (!x) n++;
                }
            }
        }
        f->free_clus = n;
        f->free_known = 1;
    }
    *free_bytes = f->free_clus * f->csize;
    return 1;
}
