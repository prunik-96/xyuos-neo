#ifndef FATFS_H
#define FATFS_H

#include <stdint.h>

// FAT16, FAT32 and exFAT, read and written, with long names (UTF-8 here,
// UTF-16 on the disk). Names are matched without regard to case, as Windows
// does; the case they were written in is kept.
//
// One driver for the three: they share the cluster heap and the FAT, and
// differ in their directory entries (8.3 plus long-name entries; exFAT's
// entry sets) and in how free clusters are found (the FAT itself; exFAT's
// allocation bitmap).

struct vol;

// A directory: where its entries are, and where its own entry is (exFAT keeps
// a directory's length there, and it grows).
struct fdir {
    uint32_t first;            // first cluster; 0 with `fixed` = FAT16's root region
    int contig;                // exFAT NoFatChain: its clusters lie in a row
    uint64_t size;             // exFAT: its length; FAT: 0 (the chain says)
    int fixed;                 // FAT16's root: a region of its own, never grows
    int root;
    // Its own entry: in the directory up_* at index up_ent (not for the root).
    uint32_t up_first;
    int up_contig, up_fixed;
    uint64_t up_size;
    int up_ent;
};

// An open file or directory.
struct ffile {
    struct fdir in;            // the directory its entry is in
    int ent, nents;            // its entries there: the first one, and how many
    uint32_t first;            // first cluster, 0 when it has none
    uint64_t size;
    uint64_t vdl;              // exFAT: how much was ever written; past it reads as zeros
    int contig;                // exFAT NoFatChain
    int is_dir;
    uint8_t attr;
    uint32_t mdos;             // modified: date << 16 | time, as FAT packs it
    uint32_t ncl, lastc;       // clusters it has and its last, once counted (ncl 0: not yet)
    int dirty;                 // size or clusters changed: the entry needs writing
    uint64_t cpos;             // cursor: cluster `cclus` holds the byte at cpos
    uint32_t cclus;
};

// Called by the volume layer with the volume's first sector. 1 if this is
// a FAT or exFAT it can use.
int  fatfs_mount(struct vol *v, const uint8_t *boot);
void fatfs_unmount(struct vol *v);
const char *fatfs_kind(struct vol *v);                 // "FAT16", "FAT32", "exFAT"
int  fatfs_label(struct vol *v, char *out, int max);
int  fatfs_size(struct vol *v, uint64_t *total);
int  fatfs_free(struct vol *v, uint64_t *free_bytes);  // counts the first time

// Find `path` ("/" is the root). 1 if it is there.
int  fatfs_open(struct vol *v, const char *path, struct ffile *f);
// Make a new, empty file or directory at `path`. 0 if it exists already, the
// directory it would go in does not, or there is no room.
int  fatfs_create(struct vol *v, const char *path, int is_dir, struct ffile *f);
long fatfs_pread(struct vol *v, struct ffile *f, uint64_t off, void *buf, uint32_t len);
// Write at `off`, growing the file as needed (a gap before it reads as zeros).
long fatfs_pwrite(struct vol *v, struct ffile *f, uint64_t off, const void *buf, uint32_t len);
int  fatfs_truncate(struct vol *v, struct ffile *f, uint64_t len);
// Write the file's entry back (its size, its clusters, the time) if it changed.
int  fatfs_sync(struct vol *v, struct ffile *f);

typedef void (*fatfs_list_cb)(const char *name, int is_dir, uint64_t size,
                              int64_t mtime, void *ctx);
int  fatfs_list(struct vol *v, const char *path, fatfs_list_cb cb, void *ctx);
// Remove a file, or a directory with nothing in it.
int  fatfs_unlink(struct vol *v, const char *path);
// Rename or move, on the same volume. 0 if the target exists.
int  fatfs_rename(struct vol *v, const char *from, const char *to);
// When the entry was last written, as seconds since 1970 (local time).
int64_t fatfs_mtime(struct vol *v, struct ffile *f);

#endif
