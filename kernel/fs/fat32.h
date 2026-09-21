#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

// A small read (and later write) FAT32 driver for the USB flash drive. Sits on
// top of the USB mass-storage block layer (usb_disk_read/write). Short 8.3 names
// only for now -- long-filename (LFN) entries are skipped.

// One directory entry surfaced to callers (8.3 name, uppercased as stored).
struct fat_dirent {
    char     name[13];   // "NAME.EXT\0"
    uint32_t size;       // bytes (0 for directories)
    uint32_t cluster;    // first cluster
    int      is_dir;
};

// Mount the FAT32 filesystem on USB disk index `dev`. Returns 1 on success.
int fat32_mount(int dev);
// Try every USB disk and mount the first that is FAT32 (skips the ISO9660 boot
// stick). Returns 1 if one mounted. Use this to find the data stick.
int fat32_automount(void);
int fat32_mounted(void);

// List the directory at `path` ("/" = root). Calls `cb(&ent, ctx)` for each
// entry. Returns 1 on success, 0 if the path is not a directory / not found.
int fat32_list(const char *path, void (*cb)(const struct fat_dirent *, void *), void *ctx);

// Look up `path`; fills *out. Returns 1 if found.
int fat32_stat(const char *path, struct fat_dirent *out);

// Read up to `maxlen` bytes of the file at `path` into `buf` starting at byte
// `offset`. Returns the number of bytes read, or -1 on error.
long fat32_read(const char *path, uint32_t offset, void *buf, uint32_t maxlen);

// Create or overwrite the file at `path` with `len` bytes from `data`. The
// parent directory must already exist; 8.3 names only. Returns bytes written,
// or -1 on error (bad path, missing parent, name is a directory, disk full).
long fat32_write(const char *path, const void *data, uint32_t len);

// Create a directory (with its "." and ".." entries). 0 on success, -1 if the
// parent is missing, the name is taken, or the disk is full.
long fat32_mkdir(const char *path);

// Remove a file, or an EMPTY directory: frees its clusters and tombstones the
// entry. 0 on success, -1 if missing or a non-empty directory.
long fat32_unlink(const char *path);

// Rename/move within the stick. Metadata only -- no file data is copied.
// 0 on success, -1 if the source is missing or the target name already exists.
long fat32_rename(const char *oldpath, const char *newpath);

#endif
