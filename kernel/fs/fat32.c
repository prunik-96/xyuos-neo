// FAT32 reader over the USB mass-storage block layer. See fat32.h.
//
// Layout: [reserved sectors][FAT x N][data region]. A cluster is sec_per_clus
// sectors; cluster C starts at data_start + (C-2)*sec_per_clus. The FAT is an
// array of 32-bit entries (28 significant bits): entry C -> next cluster of the
// chain, or >= 0x0FFFFFF8 for end-of-chain. Directories are just files whose
// data is an array of 32-byte entries.

#include "fat32.h"
#include <stddef.h>
#include "../drivers/xhci.h"      // usb_disk_read/write
#include "../drivers/rtc.h"       // real timestamps on written entries
#include "../kernel/kio.h"

static int      mounted;
static int      fat_dev = -1;      // which USB disk we are mounted on
static uint32_t bytes_per_sec, sec_per_clus, reserved_sec, num_fats, fat_size, root_clus;
static uint32_t first_fat_sec, first_data_sec;
static uint32_t tot_sec, count_clus, fsinfo_sec;   // for the write path
static uint32_t alloc_hint = 2;                    // where to start scanning for free clusters

// Three independent sector buffers: dir/file data, FAT lookups, and a data
// staging buffer for writes -- so a write never clobbers a dir or FAT sector
// held in another buffer mid-operation.
static uint8_t dbuf[512];
static uint8_t fbuf[512];
static uint8_t wbuf[512];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

int fat32_mounted(void) { return mounted; }

int fat32_mount(int dev) {
    mounted = 0;
    fat_dev = dev;
    if (dev < 0 || dev >= usb_disk_count()) return 0;
    if (!usb_disk_read(dev, 0, 1, dbuf)) return 0;
    if (dbuf[510] != 0x55 || dbuf[511] != 0xAA) return 0;
    bytes_per_sec = rd16(dbuf + 11);
    sec_per_clus  = dbuf[13];
    reserved_sec  = rd16(dbuf + 14);
    num_fats      = dbuf[16];
    fat_size      = rd32(dbuf + 36);   // BPB_FATSz32
    root_clus     = rd32(dbuf + 44);   // BPB_RootClus
    if (bytes_per_sec != 512 || sec_per_clus == 0 || fat_size == 0 || root_clus < 2)
        return 0;
    // BS_FilSysType at offset 82 must read "FAT32   " -- this is what rejects the
    // ISO9660 boot stick (which also has a 0x55AA signature).
    if (dbuf[82] != 'F' || dbuf[83] != 'A' || dbuf[84] != 'T' || dbuf[85] != '3')
        return 0;
    first_fat_sec  = reserved_sec;
    first_data_sec = reserved_sec + num_fats * fat_size;
    // Total sectors: BPB_TotSec16 (offset 19) if nonzero, else BPB_TotSec32 (32).
    tot_sec = rd16(dbuf + 19);
    if (tot_sec == 0) tot_sec = rd32(dbuf + 32);
    fsinfo_sec = rd16(dbuf + 48);          // BPB_FSInfo
    count_clus = (tot_sec - first_data_sec) / sec_per_clus;  // #data clusters
    alloc_hint = 2;
    mounted = 1;
    return 1;
}

int fat32_automount(void) {
    int n = usb_disk_count();
    for (int d = 0; d < n; d++)
        if (fat32_mount(d)) return 1;
    return 0;
}

static uint32_t clus_to_sec(uint32_t c) { return first_data_sec + (c - 2) * sec_per_clus; }

// Next cluster in the chain (or >= 0x0FFFFFF8 = end). Uses fbuf.
static uint32_t fat_next(uint32_t c) {
    uint32_t off = c * 4;
    uint32_t sec = first_fat_sec + off / bytes_per_sec;
    if (!usb_disk_read(fat_dev, sec, 1, fbuf)) return 0x0FFFFFFF;
    return rd32(fbuf + (off % bytes_per_sec)) & 0x0FFFFFFF;
}

// Turn "hello.txt" into the 11-byte space-padded 8.3 field "HELLO   TXT".
static void to_83(const char *name, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8) out[o++] = upper(name[i++]);
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') { i++; int e = 8; while (name[i] && e < 11) out[e++] = upper(name[i++]); }
}

// Turn the 11-byte 8.3 field into a friendly "NAME.EXT" string.
static void from_83(const uint8_t *raw, char out[13]) {
    int o = 0;
    for (int i = 0; i < 8; i++) { if (raw[i] == ' ') break; out[o++] = raw[i]; }
    int has_ext = raw[8] != ' ' || raw[9] != ' ' || raw[10] != ' ';
    if (has_ext) {
        out[o++] = '.';
        for (int i = 8; i < 11; i++) { if (raw[i] == ' ') break; out[o++] = raw[i]; }
    }
    out[o] = 0;
}

static int eq11(const uint8_t *a, const char *b) {
    for (int i = 0; i < 11; i++) if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}

// Walk the directory whose data starts at `dir_clus`. For each live entry call
// visit(entry_raw, ctx); if visit returns non-zero, stop and return 1.
static int walk_dir(uint32_t dir_clus, int (*visit)(const uint8_t *, void *), void *ctx) {
    uint32_t c = dir_clus;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t base = clus_to_sec(c);
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (!usb_disk_read(fat_dev, base + s, 1, dbuf)) return 0;
            for (int e = 0; e < 512; e += 32) {
                const uint8_t *d = dbuf + e;
                if (d[0] == 0x00) return 0;          // end of directory
                if (d[0] == 0xE5) continue;          // deleted
                if (d[11] == 0x0F) continue;         // long-filename entry
                if (d[11] & 0x08) continue;          // volume label
                if (visit(d, ctx)) return 1;
            }
        }
        c = fat_next(c);
    }
    return 0;
}

// --- path lookup ------------------------------------------------------------

struct find_ctx { const char *want83; struct fat_dirent *out; int found; };

static int find_visit(const uint8_t *d, void *vctx) {
    struct find_ctx *fc = (struct find_ctx *)vctx;
    if (!eq11(d, fc->want83)) return 0;
    from_83(d, fc->out->name);
    fc->out->cluster = ((uint32_t)rd16(d + 20) << 16) | rd16(d + 26);
    fc->out->size    = rd32(d + 28);
    fc->out->is_dir  = (d[11] & 0x10) ? 1 : 0;
    fc->found = 1;
    return 1;
}

int fat32_stat(const char *path, struct fat_dirent *out) {
    if (!mounted) return 0;
    // Start at root.
    out->cluster = root_clus; out->size = 0; out->is_dir = 1; out->name[0] = 0;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return 1;   // "/" itself

    uint32_t dir = root_clus;
    while (*p) {
        char comp[64]; int ci = 0;
        while (*p && *p != '/' && ci < 63) comp[ci++] = *p++;
        comp[ci] = 0;
        while (*p == '/') p++;

        char want[11]; to_83(comp, want);
        struct find_ctx fc = { want, out, 0 };
        walk_dir(dir, find_visit, &fc);
        if (!fc.found) return 0;
        dir = out->cluster;
        if (*p && !out->is_dir) return 0;   // path continues but this is a file
    }
    return 1;
}

// --- listing ----------------------------------------------------------------

struct list_ctx { void (*cb)(const struct fat_dirent *, void *); void *user; };

static int list_visit(const uint8_t *d, void *vctx) {
    struct list_ctx *lc = (struct list_ctx *)vctx;
    struct fat_dirent ent;
    from_83(d, ent.name);
    ent.cluster = ((uint32_t)rd16(d + 20) << 16) | rd16(d + 26);
    ent.size    = rd32(d + 28);
    ent.is_dir  = (d[11] & 0x10) ? 1 : 0;
    lc->cb(&ent, lc->user);
    return 0;   // keep going
}

int fat32_list(const char *path, void (*cb)(const struct fat_dirent *, void *), void *ctx) {
    if (!mounted) return 0;
    struct fat_dirent dir;
    if (!fat32_stat(path, &dir)) return 0;
    if (!dir.is_dir) return 0;
    struct list_ctx lc = { cb, ctx };
    walk_dir(dir.cluster, list_visit, &lc);
    return 1;
}

// --- file read --------------------------------------------------------------

long fat32_read(const char *path, uint32_t offset, void *buf, uint32_t maxlen) {
    if (!mounted) return -1;
    struct fat_dirent f;
    if (!fat32_stat(path, &f) || f.is_dir) return -1;
    if (offset >= f.size) return 0;
    uint32_t remain = f.size - offset;
    if (maxlen > remain) maxlen = remain;

    uint32_t clus_bytes = sec_per_clus * bytes_per_sec;
    uint32_t skip_clus = offset / clus_bytes;
    uint32_t clus = f.cluster;
    for (uint32_t i = 0; i < skip_clus && clus < 0x0FFFFFF8; i++) clus = fat_next(clus);

    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;
    uint32_t pos = offset % clus_bytes;   // byte offset within the current cluster
    while (got < maxlen && clus >= 2 && clus < 0x0FFFFFF8) {
        uint32_t base = clus_to_sec(clus);
        for (uint32_t s = 0; s < sec_per_clus && got < maxlen; s++) {
            if (pos >= (s + 1) * bytes_per_sec) continue;   // sector fully before pos
            if (!usb_disk_read(fat_dev, base + s, 1, dbuf)) return (long)got;
            uint32_t sec_start = s * bytes_per_sec;
            uint32_t from = (pos > sec_start) ? (pos - sec_start) : 0;
            for (uint32_t b = from; b < bytes_per_sec && got < maxlen; b++)
                out[got++] = dbuf[b];
        }
        pos = 0;
        clus = fat_next(clus);
    }
    return (long)got;
}

// --- file write -------------------------------------------------------------
//
// Writes go: allocate a fresh cluster chain for the data, then create or update
// the 8.3 directory entry to point at it (freeing any old chain the name had).
// Doing the data first means a disk-full failure leaves the directory untouched.
// Short 8.3 names only; the parent directory must already exist.

#define FAT_EOC 0x0FFFFFFFu

// Read one FAT entry (28-bit). Uses fbuf.
static uint32_t fat_get(uint32_t clus) {
    uint32_t off = clus * 4;
    uint32_t sec = first_fat_sec + off / bytes_per_sec;
    if (!usb_disk_read(fat_dev, sec, 1, fbuf)) return FAT_EOC;
    return rd32(fbuf + off % bytes_per_sec) & 0x0FFFFFFF;
}

// Write one FAT entry into ALL FAT copies (preserving the top 4 reserved bits).
static int fat_set(uint32_t clus, uint32_t val) {
    uint32_t off = clus * 4;
    uint32_t within = off % bytes_per_sec;
    uint32_t sec_in_fat = off / bytes_per_sec;
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t sec = first_fat_sec + f * fat_size + sec_in_fat;
        if (!usb_disk_read(fat_dev, sec, 1, fbuf)) return 0;
        uint32_t cur = rd32(fbuf + within);
        wr32(fbuf + within, (cur & 0xF0000000u) | (val & 0x0FFFFFFFu));
        if (!usb_disk_write(fat_dev, sec, 1, fbuf)) return 0;
    }
    return 1;
}

// Find a free cluster, mark it end-of-chain, and return it (0 = disk full).
static uint32_t alloc_cluster(void) {
    for (uint32_t i = 0; i < count_clus; i++) {
        uint32_t c = 2 + ((alloc_hint - 2 + i) % count_clus);
        if (fat_get(c) == 0) {
            if (!fat_set(c, FAT_EOC)) return 0;
            alloc_hint = c + 1;
            return c;
        }
    }
    return 0;
}

// Free a whole cluster chain (marks every entry 0).
static void free_chain(uint32_t clus) {
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        uint32_t next = fat_get(clus);
        fat_set(clus, 0);
        clus = next;
    }
}

// Zero every sector of a cluster (used when extending a directory). Uses wbuf.
static int zero_cluster(uint32_t c) {
    for (int i = 0; i < 512; i++) wbuf[i] = 0;
    uint32_t base = clus_to_sec(c);
    for (uint32_t s = 0; s < sec_per_clus; s++)
        if (!usb_disk_write(fat_dev, base + s, 1, wbuf)) return 0;
    return 1;
}

// Allocate + fill a cluster chain with `len` bytes of `data`. Returns the first
// cluster (0 for an empty file, which needs none; 0 on disk-full too -- callers
// distinguish by len). Uses wbuf for staging; leaves cluster slack past the end
// untouched (it is beyond EOF, exactly as any FAT implementation does).
static uint32_t write_data_chain(const uint8_t *data, uint32_t len) {
    if (len == 0) return 0;
    uint32_t first = 0, prev = 0, off = 0;
    while (off < len) {
        uint32_t c = alloc_cluster();
        if (c == 0) { if (first) free_chain(first); return 0; }
        if (prev) { if (!fat_set(prev, c)) { free_chain(first); return 0; } }
        else      { first = c; }

        uint32_t base = clus_to_sec(c);
        for (uint32_t s = 0; s < sec_per_clus && off < len; s++) {
            uint32_t chunk = bytes_per_sec;
            if (chunk > len - off) chunk = len - off;
            for (uint32_t i = 0; i < chunk; i++) wbuf[i] = data[off + i];
            for (uint32_t i = chunk; i < bytes_per_sec; i++) wbuf[i] = 0;
            if (!usb_disk_write(fat_dev, base + s, 1, wbuf)) { free_chain(first); return 0; }
            off += chunk;
        }
        prev = c;
    }
    return first;   // last cluster is already EOC from alloc_cluster
}

// Current time packed into FAT's date/time words (date: y-1980<<9 | mon<<5 | day;
// time: hour<<11 | min<<5 | sec/2). Falls back to 1980-01-01 if the RTC is odd.
static void fat_now(uint16_t *date, uint16_t *time) {
    struct rtc_time t;
    rtc_read(&t);
    int yr = t.year - 1980;
    if (yr < 0 || yr > 127 || t.mon < 1 || t.mon > 12 || t.day < 1 || t.day > 31) {
        *date = (1 << 5) | 1;   // 1980-01-01
        *time = 0;
        return;
    }
    *date = (uint16_t)((yr << 9) | (t.mon << 5) | t.day);
    *time = (uint16_t)(((t.hour & 0x1F) << 11) | ((t.min & 0x3F) << 5) | ((t.sec / 2) & 0x1F));
}

// Lay a directory entry into the sector already in dbuf at byte `off`.
static int write_dir_entry(uint32_t sec, uint32_t off, const char name83[11],
                           uint32_t first_clus, uint32_t size, int is_dir) {
    if (!usb_disk_read(fat_dev, sec, 1, dbuf)) return 0;
    uint8_t *d = dbuf + off;
    for (int i = 0; i < 11; i++) d[i] = (uint8_t)name83[i];
    d[11] = is_dir ? 0x10 : 0x20;              // ATTR_DIRECTORY : ATTR_ARCHIVE
    for (int i = 12; i < 32; i++) d[i] = 0;    // NT-res etc.
    uint16_t date, time;
    fat_now(&date, &time);
    wr16(d + 14, time);   // DIR_CrtTime
    wr16(d + 16, date);   // DIR_CrtDate
    wr16(d + 18, date);   // DIR_LstAccDate
    wr16(d + 22, time);   // DIR_WrtTime
    wr16(d + 24, date);   // DIR_WrtDate
    wr16(d + 20, (uint16_t)(first_clus >> 16));    // DIR_FstClusHI
    wr16(d + 26, (uint16_t)(first_clus & 0xFFFF)); // DIR_FstClusLO
    wr32(d + 28, size);                            // DIR_FileSize
    return usb_disk_write(fat_dev, sec, 1, dbuf);
}

// Signal "free count unknown" in FSInfo so the host recomputes it (cheaper and
// safer than tracking it ourselves). Best-effort: failures are ignored.
static void fsinfo_invalidate(void) {
    if (fsinfo_sec == 0 || fsinfo_sec >= reserved_sec) return;
    if (!usb_disk_read(fat_dev, fsinfo_sec, 1, fbuf)) return;
    if (rd32(fbuf + 0) != 0x41615252) return;      // FSI_LeadSig sanity
    wr32(fbuf + 488, 0xFFFFFFFF);                  // FSI_Free_Count = unknown
    wr32(fbuf + 492, 0xFFFFFFFF);                  // FSI_Nxt_Free   = unknown
    usb_disk_write(fat_dev, fsinfo_sec, 1, fbuf);
}

// Split "/dir/leaf" into parent path (into `parent`) and leaf name (into `leaf`).
// "/foo" -> parent "/", leaf "foo". Returns 0 if there is no leaf.
static int split_path(const char *path, char *parent, char *leaf) {
    int len = 0; while (path[len]) len++;
    if (len > 1 && path[len - 1] == '/') len--;     // ignore a trailing slash
    int slash = -1;
    for (int i = len - 1; i >= 0; i--) if (path[i] == '/') { slash = i; break; }
    if (slash < 0) { parent[0] = '/'; parent[1] = 0; }
    else if (slash == 0) { parent[0] = '/'; parent[1] = 0; }
    else { int i = 0; for (; i < slash; i++) parent[i] = path[i]; parent[i] = 0; }
    int o = 0; for (int i = slash + 1; i < len && o < 12; i++) leaf[o++] = path[i];
    leaf[o] = 0;
    return o > 0;
}

// Create or overwrite a file at `path` on the mounted stick with `len` bytes of
// `data`. Returns bytes written, or -1 on error (bad path, parent missing, name
// is a directory, or disk full).
long fat32_write(const char *path, const void *data, uint32_t len) {
    if (!mounted) return -1;

    char parent[256], leaf[16];
    if (!split_path(path, parent, leaf)) return -1;

    struct fat_dirent pd;
    if (!fat32_stat(parent, &pd) || !pd.is_dir) return -1;
    uint32_t dir = pd.cluster ? pd.cluster : root_clus;

    char name83[11];
    to_83(leaf, name83);

    // Scan the parent directory: look for an existing entry with this name, and
    // remember the first reusable slot (a deleted 0xE5 entry, or the 0x00
    // terminator) plus the last cluster in case we must extend.
    uint32_t c = dir, last_c = dir;
    int have_exist = 0, exist_is_dir = 0; uint32_t exist_sec = 0, exist_off = 0, old_first = 0;
    int have_free = 0, free_is_term = 0; uint32_t free_sec = 0, free_off = 0;
    int stop = 0;
    while (c >= 2 && c < 0x0FFFFFF8 && !stop) {
        last_c = c;
        uint32_t base = clus_to_sec(c);
        for (uint32_t s = 0; s < sec_per_clus && !stop; s++) {
            if (!usb_disk_read(fat_dev, base + s, 1, dbuf)) return -1;
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = dbuf + e;
                if (d[0] == 0x00) {
                    if (!have_free) { have_free = 1; free_is_term = 1; free_sec = base + s; free_off = e; }
                    stop = 1; break;                 // nothing live past here
                }
                if (d[0] == 0xE5) {
                    if (!have_free) { have_free = 1; free_is_term = 0; free_sec = base + s; free_off = e; }
                    continue;
                }
                if (d[11] == 0x0F) continue;         // LFN fragment
                if (eq11(d, name83)) {
                    have_exist = 1; exist_sec = base + s; exist_off = e;
                    old_first = ((uint32_t)rd16(d + 20) << 16) | rd16(d + 26);
                    exist_is_dir = (d[11] & 0x10) ? 1 : 0;
                    stop = 1; break;
                }
            }
        }
        if (!stop) c = fat_get(c);
    }

    if (have_exist && exist_is_dir) return -1;       // refuse to clobber a directory

    // Data first, so an out-of-space failure never leaves a dangling dir entry.
    uint32_t new_first = write_data_chain((const uint8_t *)data, len);
    if (len > 0 && new_first == 0) return -1;

    if (have_exist) {
        if (old_first >= 2) free_chain(old_first);
        if (!write_dir_entry(exist_sec, exist_off, name83, new_first, len, 0)) return -1;
        fsinfo_invalidate();
        return (long)len;
    }

    // New file: find a slot to write the entry into.
    uint32_t slot_sec, slot_off;
    if (have_free) {
        slot_sec = free_sec; slot_off = free_off;
        // Consuming the 0x00 terminator at the very last slot of the last cluster
        // would leave no terminator: extend the directory with a fresh cluster.
        uint32_t last_slot_sec = clus_to_sec(last_c) + sec_per_clus - 1;
        if (free_is_term && slot_sec == last_slot_sec && slot_off == 512 - 32) {
            uint32_t nc = alloc_cluster();
            if (nc == 0 || !zero_cluster(nc) || !fat_set(last_c, nc)) {
                if (new_first) free_chain(new_first);
                return -1;
            }
        }
    } else {
        // Directory full to the end of its chain with no terminator: extend.
        uint32_t nc = alloc_cluster();
        if (nc == 0 || !zero_cluster(nc) || !fat_set(last_c, nc)) {
            if (new_first) free_chain(new_first);
            return -1;
        }
        slot_sec = clus_to_sec(nc); slot_off = 0;
    }

    if (!write_dir_entry(slot_sec, slot_off, name83, new_first, len, 0)) return -1;
    fsinfo_invalidate();
    return (long)len;
}

// --- directory-entry helpers (mkdir / unlink / rename) ----------------------
//
// fat32_write above keeps its own inlined scan because it has to find an
// existing entry AND a free slot in one pass; these two helpers split those
// jobs for the operations that only need one of them.

// Locate the entry named `name83` in directory `dir_clus`, copying the raw
// 32-byte entry out (dbuf is reused by later reads, so callers need a copy).
static int find_entry_loc(uint32_t dir_clus, const char name83[11],
                          uint32_t *out_sec, uint32_t *out_off, uint8_t out_entry[32]) {
    uint32_t c = dir_clus;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t base = clus_to_sec(c);
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (!usb_disk_read(fat_dev, base + s, 1, dbuf)) return 0;
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = dbuf + e;
                if (d[0] == 0x00) return 0;            // end of directory
                if (d[0] == 0xE5 || d[11] == 0x0F) continue;
                if (eq11(d, name83)) {
                    *out_sec = base + s;
                    *out_off = (uint32_t)e;
                    if (out_entry) for (int i = 0; i < 32; i++) out_entry[i] = d[i];
                    return 1;
                }
            }
        }
        c = fat_get(c);
    }
    return 0;
}

// Find a slot for a NEW entry in `dir_clus`, growing the directory when needed.
static int alloc_entry_slot(uint32_t dir_clus, uint32_t *out_sec, uint32_t *out_off) {
    uint32_t c = dir_clus, last_c = dir_clus;
    while (c >= 2 && c < 0x0FFFFFF8) {
        last_c = c;
        uint32_t base = clus_to_sec(c);
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (!usb_disk_read(fat_dev, base + s, 1, dbuf)) return 0;
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = dbuf + e;
                if (d[0] == 0xE5) { *out_sec = base + s; *out_off = (uint32_t)e; return 1; }
                if (d[0] == 0x00) {
                    // Consuming the very last slot would leave the directory with
                    // no terminator, so give it a fresh (zeroed) cluster first.
                    if (base + s == clus_to_sec(last_c) + sec_per_clus - 1 && e == 512 - 32) {
                        uint32_t nc = alloc_cluster();
                        if (!nc || !zero_cluster(nc) || !fat_set(last_c, nc)) return 0;
                    }
                    *out_sec = base + s; *out_off = (uint32_t)e;
                    return 1;
                }
            }
        }
        c = fat_get(c);
    }
    // Chain ended with no terminator at all: extend.
    uint32_t nc = alloc_cluster();
    if (!nc || !zero_cluster(nc) || !fat_set(last_c, nc)) return 0;
    *out_sec = clus_to_sec(nc);
    *out_off = 0;
    return 1;
}

// 1 if the directory holds nothing but its "." and ".." entries.
static int dir_is_empty(uint32_t clus) {
    uint32_t c = clus;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t base = clus_to_sec(c);
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (!usb_disk_read(fat_dev, base + s, 1, dbuf)) return 0;
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = dbuf + e;
                if (d[0] == 0x00) return 1;
                if (d[0] == 0xE5 || d[11] == 0x0F) continue;
                if (d[0] == '.' && (d[1] == ' ' || (d[1] == '.' && d[2] == ' '))) continue;
                return 0;
            }
        }
        c = fat_get(c);
    }
    return 1;
}

long fat32_mkdir(const char *path) {
    if (!mounted) return -1;
    char parent[256], leaf[16];
    if (!split_path(path, parent, leaf)) return -1;

    struct fat_dirent pd;
    if (!fat32_stat(parent, &pd) || !pd.is_dir) return -1;
    uint32_t dir = pd.cluster ? pd.cluster : root_clus;

    char name83[11];
    to_83(leaf, name83);
    uint32_t sec, off;
    if (find_entry_loc(dir, name83, &sec, &off, NULL)) return -1;   // name taken

    // A directory's data is one zeroed cluster holding "." and "..". Per the
    // spec ".." carries cluster 0 when the parent is the root directory.
    uint32_t nc = alloc_cluster();
    if (!nc || !zero_cluster(nc)) return -1;

    char dot[11], dotdot[11];
    for (int i = 0; i < 11; i++) { dot[i] = ' '; dotdot[i] = ' '; }
    dot[0] = '.';
    dotdot[0] = '.'; dotdot[1] = '.';
    uint32_t base = clus_to_sec(nc);
    uint32_t parent_clus = (dir == root_clus) ? 0 : dir;
    if (!write_dir_entry(base, 0,  dot,    nc,           0, 1) ||
        !write_dir_entry(base, 32, dotdot, parent_clus,  0, 1)) {
        free_chain(nc);
        return -1;
    }

    if (!alloc_entry_slot(dir, &sec, &off) ||
        !write_dir_entry(sec, off, name83, nc, 0, 1)) {
        free_chain(nc);
        return -1;
    }
    fsinfo_invalidate();
    return 0;
}

long fat32_unlink(const char *path) {
    if (!mounted) return -1;
    char parent[256], leaf[16];
    if (!split_path(path, parent, leaf)) return -1;

    struct fat_dirent pd;
    if (!fat32_stat(parent, &pd) || !pd.is_dir) return -1;
    uint32_t dir = pd.cluster ? pd.cluster : root_clus;

    char name83[11];
    to_83(leaf, name83);
    uint32_t sec, off;
    uint8_t ent[32];
    if (!find_entry_loc(dir, name83, &sec, &off, ent)) return -1;

    uint32_t first = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
    if (ent[11] & 0x10) {                       // directory: only if empty
        if (first < 2 || !dir_is_empty(first)) return -1;
    }
    if (first >= 2) free_chain(first);

    if (!usb_disk_read(fat_dev, sec, 1, dbuf)) return -1;
    dbuf[off] = 0xE5;                           // tombstone the entry
    if (!usb_disk_write(fat_dev, sec, 1, dbuf)) return -1;
    fsinfo_invalidate();
    return 0;
}

long fat32_rename(const char *oldpath, const char *newpath) {
    if (!mounted) return -1;
    char op[256], ol[16], np[256], nl[16];
    if (!split_path(oldpath, op, ol)) return -1;
    if (!split_path(newpath, np, nl)) return -1;

    struct fat_dirent od, nd;
    if (!fat32_stat(op, &od) || !od.is_dir) return -1;
    if (!fat32_stat(np, &nd) || !nd.is_dir) return -1;
    uint32_t odir = od.cluster ? od.cluster : root_clus;
    uint32_t ndir = nd.cluster ? nd.cluster : root_clus;

    char on83[11], nn83[11];
    to_83(ol, on83);
    to_83(nl, nn83);

    uint32_t osec, ooff;
    uint8_t ent[32];
    if (!find_entry_loc(odir, on83, &osec, &ooff, ent)) return -1;
    uint32_t tsec, toff;
    if (find_entry_loc(ndir, nn83, &tsec, &toff, NULL)) return -1;   // target exists

    // Pure metadata move: the new entry keeps the same first cluster, size and
    // attributes, so no file data is copied or freed.
    if (!alloc_entry_slot(ndir, &tsec, &toff)) return -1;
    if (!usb_disk_read(fat_dev, tsec, 1, dbuf)) return -1;
    for (int i = 0; i < 32; i++) dbuf[toff + i] = ent[i];
    for (int i = 0; i < 11; i++) dbuf[toff + i] = (uint8_t)nn83[i];
    if (!usb_disk_write(fat_dev, tsec, 1, dbuf)) return -1;

    // A directory that changed parent must have its ".." repointed.
    if ((ent[11] & 0x10) && odir != ndir) {
        uint32_t self = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
        if (self >= 2) {
            uint32_t base = clus_to_sec(self);
            if (usb_disk_read(fat_dev, base, 1, dbuf)) {
                uint32_t pc = (ndir == root_clus) ? 0 : ndir;
                wr16(dbuf + 32 + 20, (uint16_t)(pc >> 16));
                wr16(dbuf + 32 + 26, (uint16_t)(pc & 0xFFFF));
                usb_disk_write(fat_dev, base, 1, dbuf);
            }
        }
    }

    // Drop the old entry last: until this point a failure leaves the file
    // reachable under its original name.
    if (!usb_disk_read(fat_dev, osec, 1, dbuf)) return -1;
    dbuf[ooff] = 0xE5;
    if (!usb_disk_write(fat_dev, osec, 1, dbuf)) return -1;
    fsinfo_invalidate();
    return 0;
}
