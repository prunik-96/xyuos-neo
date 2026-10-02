// Volumes on USB drives. See vol.h.

#include "vol.h"
#include "fatfs.h"
#include "../drivers/xhci.h"
#include "../drivers/blkdev.h"
#include "../kernel/kio.h"
#include "../mm/heap.h"
#include <stddef.h>

struct vol {
    int used;
    int index;                   // its place in vols[]: its mount point
    uint32_t gen;                // bumped whenever the place is taken or freed
    int disk;                    // which USB disk
    uint32_t disk_id;            // ... and which plugging-in of it
    uint64_t first, count;       // where it is on the disk, in sectors
    int type;
    char label[VOL_NAME_MAX];
    char fsname[8];
    void *fs;                    // the driver's state
    uint64_t total, free_bytes;
    int free_known;
};

static struct vol vols[VOL_MAX];
#define MAX_DISKS 8
static uint32_t scanned[MAX_DISKS];   // the disk id each disk index was last scanned as

// --- the sector cache ----------------------------------------------------------
// Directories and the FAT are read and written a sector at a time, again and
// again; each of those is a whole USB command. Write-through: the drive always
// has what the cache has, so a drive pulled out loses nothing it was told.

#define CACHE_N 64
struct cent {
    struct vol *v;
    uint64_t sec;
    uint32_t age;
    uint8_t data[512];
};
static struct cent cache[CACHE_N];
static uint32_t cache_clock;

// Transfers go through this: the controller writes to physical memory, and a
// program's buffer is not where its address says. 64 KiB, aligned so no
// transfer crosses a 64 KiB line.
#define BOUNCE 65536
__attribute__((aligned(65536))) static uint8_t bounce[BOUNCE];

static int disk_rw(struct vol *v, uint64_t sec, uint32_t n, void *buf, int write) {
    if (sec + n > v->count || sec + n < sec) return 0;
    uint8_t *b = (uint8_t *)buf;
    while (n) {
        uint32_t k = n > BOUNCE / 512 ? BOUNCE / 512 : n;
        uint64_t at = v->first + sec;
        if (at + k > 0xFFFFFFFFull) return 0;            // READ(10) reaches 2 TiB
        if (write) {
            for (uint32_t i = 0; i < k * 512; i++) bounce[i] = b[i];
            if (!usb_disk_write(v->disk, (uint32_t)at, k, bounce)) return 0;
        } else {
            if (!usb_disk_read(v->disk, (uint32_t)at, k, bounce)) return 0;
            for (uint32_t i = 0; i < k * 512; i++) b[i] = bounce[i];
        }
        b += k * 512;
        sec += k;
        n -= k;
    }
    return 1;
}

static struct cent *cache_find(struct vol *v, uint64_t sec) {
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].v == v && cache[i].sec == sec) return &cache[i];
    return NULL;
}

static void cache_drop(struct vol *v) {
    for (int i = 0; i < CACHE_N; i++) if (cache[i].v == v) cache[i].v = NULL;
}

uint8_t *vol_sector(struct vol *v, uint64_t sec) {
    struct cent *c = cache_find(v, sec);
    if (!c) {
        c = &cache[0];
        for (int i = 0; i < CACHE_N; i++) {
            if (!cache[i].v) { c = &cache[i]; break; }
            if (cache[i].age < c->age) c = &cache[i];
        }
        c->v = NULL;
        if (!disk_rw(v, sec, 1, c->data, 0)) return NULL;
        c->v = v;
        c->sec = sec;
    }
    c->age = ++cache_clock;
    return c->data;
}

int vol_sector_put(struct vol *v, uint64_t sec) {
    struct cent *c = cache_find(v, sec);
    if (!c) return 0;
    return disk_rw(v, sec, 1, c->data, 1);
}

int vol_read(struct vol *v, uint64_t sec, uint32_t n, void *buf) {
    if (n == 1) {
        uint8_t *s = vol_sector(v, sec);
        if (!s) return 0;
        for (int i = 0; i < 512; i++) ((uint8_t *)buf)[i] = s[i];
        return 1;
    }
    if (!disk_rw(v, sec, n, buf, 0)) return 0;
    // What the cache holds is what the drive holds: nothing to reconcile.
    return 1;
}

int vol_write(struct vol *v, uint64_t sec, uint32_t n, const void *buf) {
    if (!disk_rw(v, sec, n, (void *)buf, 1)) return 0;
    // Sectors written around the cache must not be read back stale from it:
    // a directory's cluster freed and given to a file is the usual case.
    for (int i = 0; i < CACHE_N; i++) {
        struct cent *c = &cache[i];
        if (c->v == v && c->sec >= sec && c->sec < sec + n) {
            const uint8_t *src = (const uint8_t *)buf + (c->sec - sec) * 512;
            for (int k = 0; k < 512; k++) c->data[k] = src[k];
        }
    }
    return 1;
}

uint64_t vol_sectors(struct vol *v) { return v->count; }
void *vol_fs(struct vol *v) { return v->fs; }
void vol_set_fs(struct vol *v, void *fs) { v->fs = fs; }

// --- finding volumes on a disk -------------------------------------------------

static uint32_t le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static int is_fat_boot(const uint8_t *b) {
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;
    uint32_t bps = b[11] | (b[12] << 8);
    uint32_t spc = b[13];
    if (bps != 512 || spc == 0 || (spc & (spc - 1))) return 0;
    if (b[16] == 0 || (b[14] | (b[15] << 8)) == 0) return 0;      // FATs, reserved sectors
    return 1;
}

static int is_exfat_boot(const uint8_t *b) {
    static const char sig[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    for (int i = 0; i < 8; i++) if (b[3 + i] != (uint8_t)sig[i]) return 0;
    return b[510] == 0x55 && b[511] == 0xAA;
}

static void sset(char *d, const char *s, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static void mount_name(int i, char *out) {
    out[0] = 'u'; out[1] = 's'; out[2] = 'b';
    if (i == 0) { out[3] = 0; return; }
    out[3] = (char)('1' + i); out[4] = 0;        // usb2 .. usb8
}

static uint8_t sec0[512];

static void add_volume(int disk, uint64_t first, uint64_t count) {
    int i = 0;
    while (i < VOL_MAX && vols[i].used) i++;
    if (i == VOL_MAX || count < 16) return;
    struct vol *v = &vols[i];
    v->disk = disk;
    v->disk_id = usb_disk_id(disk);
    v->first = first;
    v->count = count;
    v->fs = NULL;
    v->label[0] = 0;
    v->total = count * 512;
    v->free_known = 0;
    v->index = i;
    if (!disk_rw(v, 0, 1, sec0, 0)) return;

    v->type = VOL_NONE;
    if (is_exfat_boot(sec0) || is_fat_boot(sec0)) {
        if (fatfs_mount(v, sec0)) {
            v->type = is_exfat_boot(sec0) ? VOL_EXFAT : VOL_FAT;
            sset(v->fsname, fatfs_kind(v), sizeof v->fsname);
            fatfs_label(v, v->label, sizeof v->label);
            uint64_t t, f;
            if (fatfs_size(v, &t)) v->total = t;
            (void)f;
        }
    }
    if (v->type == VOL_NONE) {
        static const char ntfs[8] = { 'N', 'T', 'F', 'S', ' ', ' ', ' ', ' ' };
        int is_ntfs = 1;
        for (int k = 0; k < 8; k++) if (sec0[3 + k] != (uint8_t)ntfs[k]) is_ntfs = 0;
        if (is_ntfs) { v->type = VOL_OTHER; sset(v->fsname, "NTFS", sizeof v->fsname); }
        else if (disk_rw(v, 2, 1, sec0, 0) && sec0[56] == 0x53 && sec0[57] == 0xEF) {
            v->type = VOL_OTHER; sset(v->fsname, "ext4", sizeof v->fsname);
        } else if (is_fat_boot(sec0)) {
            v->type = VOL_OTHER; sset(v->fsname, "FAT12", sizeof v->fsname);
        }
    }
    if (v->type == VOL_NONE) return;            // empty space, or something unknown
    v->used = 1;
    v->gen++;
    char mn[8];
    mount_name(i, mn);
    kprintf("vol: /%s on USB disk %d at %u, %u MiB, %s \"%s\"\n",
            mn, disk, (unsigned)first, (unsigned)(count / 2048), v->fsname, v->label);
}

static void scan_disk(int d) {
    uint32_t blocks = 0, bsize = 0;
    if (!usb_disk_capacity(d, &blocks, &bsize) || bsize != 512) return;
    if (!usb_disk_read(d, 0, 1, bounce)) return;
    for (int i = 0; i < 512; i++) sec0[i] = bounce[i];

    // No partition table: the filesystem starts at sector 0.
    if (is_exfat_boot(sec0) || is_fat_boot(sec0)) { add_volume(d, 0, blocks); return; }
    if (sec0[510] != 0x55 || sec0[511] != 0xAA) return;

    uint64_t f[4], n[4];
    int gpt = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec0 + 446 + 16 * i;
        f[i] = n[i] = 0;
        if (e[4] == 0xEE) gpt = 1;
        else if (e[4] && e[4] != 0x05 && e[4] != 0x0F) { f[i] = le32(e + 8); n[i] = le32(e + 12); }
    }
    if (!gpt) {
        for (int i = 0; i < 4; i++)
            if (n[i] && f[i] + n[i] <= blocks) add_volume(d, f[i], n[i]);
        return;
    }
    if (!usb_disk_read(d, 1, 1, bounce)) return;
    if (le64(bounce) != 0x5452415020494645ULL) return;                  // "EFI PART"
    uint64_t table = le64(bounce + 72);
    uint32_t entries = le32(bounce + 80), esize = le32(bounce + 84);
    if (esize < 128 || esize > 512 || entries > 256) return;
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t at = (uint64_t)i * esize;
        if (!usb_disk_read(d, (uint32_t)(table + at / 512), 1, bounce)) return;
        const uint8_t *e = bounce + at % 512;
        int used = 0;
        for (int k = 0; k < 16; k++) used |= e[k];
        if (!used) continue;
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (last < first || last >= blocks) continue;
        add_volume(d, first, last - first + 1);
    }
}

int vol_refresh(void) {
    // Volumes whose drive went, or was replaced by another in the same place.
    for (int i = 0; i < VOL_MAX; i++) {
        struct vol *v = &vols[i];
        if (!v->used || usb_disk_id(v->disk) == v->disk_id) continue;
        char mn[8];
        mount_name(i, mn);
        kprintf("vol: /%s went with its drive\n", mn);
        fatfs_unmount(v);
        cache_drop(v);
        v->used = 0;
        v->gen++;
    }
    // Drives not looked at yet. Never the one the system runs from.
    int nd = usb_disk_count();
    for (int d = 0; d < MAX_DISKS; d++) {
        uint32_t id = d < nd ? usb_disk_id(d) : 0;
        if (id == scanned[d]) continue;
        scanned[d] = id;
        if (!id || d == blkdev_usb_dev()) continue;
        scan_disk(d);
    }
    int n = 0;
    for (int i = 0; i < VOL_MAX; i++) n += vols[i].used;
    return n;
}

int vol_list(struct vol_info *out, int max) {
    vol_refresh();
    int n = 0;
    for (int i = 0; i < VOL_MAX && n < max; i++) {
        struct vol *v = &vols[i];
        if (!v->used) continue;
        struct vol_info *o = &out[n++];
        o->index = i;
        o->type = v->type;
        mount_name(i, o->mount);
        sset(o->label, v->label, sizeof o->label);
        sset(o->drive, usb_disk_name(v->disk), sizeof o->drive);
        sset(o->fs, v->fsname, sizeof o->fs);
        o->total = v->total;
        o->free = v->free_known ? v->free_bytes : 0;
        o->disk = v->disk;
    }
    return n;
}

int vol_space(int i, uint64_t *total, uint64_t *free_bytes) {
    struct vol *v = vol_get(i);
    if (!v) return 0;
    if (!v->free_known && (v->type == VOL_FAT || v->type == VOL_EXFAT)) {
        uint64_t f;
        if (fatfs_free(v, &f)) { v->free_bytes = f; v->free_known = 1; }
    }
    if (total) *total = v->total;
    if (free_bytes) *free_bytes = v->free_known ? v->free_bytes : 0;
    return 1;
}

// The free count is the driver's to keep once known: it tells us.
void vol_free_changed(struct vol *v, int64_t delta) {
    if (!v->free_known) return;
    if (delta < 0 && (uint64_t)-delta > v->free_bytes) v->free_bytes = 0;
    else v->free_bytes += delta;
}

int vol_of_path(const char *path, const char **rest) {
    if (path[0] != '/' || path[1] != 'u' || path[2] != 's' || path[3] != 'b') return -1;
    const char *p = path + 4;
    int i = 0;
    if (*p >= '2' && *p <= '9') { i = *p - '1'; p++; }
    if (*p != 0 && *p != '/') return -1;
    if (i >= VOL_MAX) return -1;
    vol_refresh();
    if (!vols[i].used) return -1;
    if (rest) *rest = *p ? p : "/";
    return i;
}

int vol_mounts(char names[][8], int max) {
    vol_refresh();
    int n = 0;
    for (int i = 0; i < VOL_MAX && n < max; i++)
        if (vols[i].used) mount_name(i, names[n++]);
    return n;
}

int vol_eject(int i) {
    if (i < 0 || i >= VOL_MAX || !vols[i].used) return 0;
    int disk = vols[i].disk;
    for (int k = 0; k < VOL_MAX; k++) {
        struct vol *v = &vols[k];
        if (!v->used || v->disk != disk) continue;
        fatfs_unmount(v);
        cache_drop(v);
        v->used = 0;
        v->gen++;
    }
    // Remembered as scanned: it is not found again until it comes back with
    // a new id -- pulled out and put in.
    return 1;
}

int vol_disk(int i) { return i >= 0 && i < VOL_MAX && vols[i].used ? vols[i].disk : -1; }

struct vol *vol_get(int i) {
    if (i < 0 || i >= VOL_MAX) return NULL;
    vol_refresh();
    return vols[i].used ? &vols[i] : NULL;
}

uint32_t vol_gen(int i) { return i >= 0 && i < VOL_MAX ? vols[i].gen : 0; }

int vol_type(int i) { return i >= 0 && i < VOL_MAX && vols[i].used ? vols[i].type : VOL_NONE; }
