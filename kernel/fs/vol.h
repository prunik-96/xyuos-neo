#ifndef VOL_H
#define VOL_H

#include <stdint.h>

// Volumes: the filesystems on USB drives, each grafted into the namespace at
// /usb, /usb2, /usb3, ... A drive with a partition table (MBR or GPT) may hold
// several; one without -- a "superfloppy" -- holds one. FAT16, FAT32 and exFAT
// are read and written, with long names; anything else (NTFS, ext4) is listed
// as a volume that cannot be opened. The boot stick is never one: every
// partition on it belongs to the system.
//
// Drives come and go. vol_refresh() notices -- it is cheap when nothing
// changed -- and every call that takes a volume calls it first.

#define VOL_MAX 8

#define VOL_NONE  0
#define VOL_FAT   1
#define VOL_EXFAT 2
#define VOL_OTHER 3              // there, but not a filesystem we read

#define VOL_NAME_MAX 64          // a label, UTF-8

// What the desktop and the file manager are told about a volume.
struct vol_info {
    int index;                   // 0 for /usb, 1 for /usb2, ...
    int type;                    // VOL_*
    char mount[8];               // "usb", "usb2", ...
    char label[VOL_NAME_MAX];    // the filesystem's own label, may be empty
    char drive[48];              // what the drive calls itself
    char fs[8];                  // "FAT32", "exFAT", "NTFS", ...
    uint64_t total, free;        // bytes; free is 0 until asked for (vol_space)
};

// Look at the drives again: forget volumes on drives that went, find the
// ones on drives that came. Returns how many volumes there are.
int vol_refresh(void);

// The volumes there are now, into out[0..max). Returns how many.
int vol_list(struct vol_info *out, int max);

// Free and total bytes of volume i (counted on the first ask: a FAT has to be
// read for it). 0 if there is no such volume.
int vol_space(int i, uint64_t *total, uint64_t *free_bytes);

// If `path` is under a volume's mount point, the volume's index and the rest
// of the path ("/usb2/a/b" -> 1, "/a/b"; "/usb" -> 0, "/"). -1 otherwise,
// including for "/usbfoo" and for a mount point with no volume behind it.
int vol_of_path(const char *path, const char **rest);

// Let go of volume i (and every other volume on its drive) so the drive can
// be pulled out: nothing is left half-written, and nothing is read from it
// again until it is plugged in anew. 1 on success.
int vol_eject(int i);
// The USB disk volume i is on, -1 if none.
int vol_disk(int i);

// The mount point names in use, for listing "/".
int vol_mounts(char names[][8], int max);

// --- for the filesystem drivers ------------------------------------------------

struct vol;
// Sectors of volume `v`, relative to its start, through a small cache. Return
// 1 on success. A write goes straight to the drive as well.
int vol_read(struct vol *v, uint64_t sec, uint32_t n, void *buf);
int vol_write(struct vol *v, uint64_t sec, uint32_t n, const void *buf);
// One cached sector: a pointer into the cache, valid until the next call; NULL
// on a read error. vol_sector_put writes it back to the drive.
uint8_t *vol_sector(struct vol *v, uint64_t sec);
int vol_sector_put(struct vol *v, uint64_t sec);
uint64_t vol_sectors(struct vol *v);
void *vol_fs(struct vol *v);     // the driver's own state
struct vol *vol_get(int i);      // volume i, refreshed; NULL if none
uint32_t vol_gen(int i);         // changes when volume i is replaced
int vol_type(int i);             // VOL_*, VOL_NONE if there is none
void vol_set_fs(struct vol *v, void *fs);
// The driver's free count changed by `delta` bytes (it allocated or freed).
void vol_free_changed(struct vol *v, int64_t delta);

#endif
