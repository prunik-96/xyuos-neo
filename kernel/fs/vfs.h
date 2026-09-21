#ifndef VFS_H
#define VFS_H

#include <stdint.h>

// `multiboot_addr` is passed through to blkdev_init so it can find the RAM-disk
// GRUB module when there is no virtio device (bare metal).
int vfs_init(uint32_t multiboot_addr);
int vfs_open(const char *path);
int32_t vfs_read(int fd, void *buf, uint32_t len);
int32_t vfs_write(int fd, const void *buf, uint32_t len);
void vfs_close(int fd);

// --- random access ---------------------------------------------------------
// Whence values match POSIX so a hosted stdio can map onto these directly.
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

// Move the read/write cursor. Returns the new absolute offset, or -1 on error
// (bad fd, unknown whence, or a resulting offset below 0). Seeking past the
// end is allowed, as in POSIX; note that writing there leaves the skipped
// range unallocated, so only write forward from the end if you mean it.
int64_t vfs_seek(int fd, int64_t offset, int whence);

// Current cursor offset, or -1 on a bad fd.
int64_t vfs_tell(int fd);

// --- metadata --------------------------------------------------------------
struct vfs_stat {
    uint32_t size;    // bytes
    uint32_t inode;   // inode number
    int      is_dir;  // 1 for a directory, 0 for a regular file
    uint32_t mtime;   // last-modified time, seconds since the Unix epoch (UTC)
};

// Stat by path / by open fd. Return 0 on success, -1 if not found or bad fd.
int vfs_stat(const char *path, struct vfs_stat *out);
int vfs_fstat(int fd, struct vfs_stat *out);

// Resize a regular file, freeing blocks past the new end. This is what lets a
// hosted stdio implement fopen(path,"w") properly instead of faking truncation
// with unlink+create. Return 0 on success, -1 on error.
int vfs_truncate(const char *path, uint32_t length);
int vfs_ftruncate(int fd, uint32_t length);

// Filesystem mutations (paths are absolute). Return 0 on success, -1 on error.
int vfs_create(const char *path);   // create empty regular file
int vfs_mkdir(const char *path);    // create directory
int vfs_unlink(const char *path);   // remove file or empty directory
int vfs_rename(const char *oldpath, const char *newpath);

// Writes one name per line into out_buf (directories get a trailing '/').
// Returns the number of bytes written.
uint32_t vfs_list_dir(const char *path, char *out_buf, uint32_t out_buf_len);

#endif
