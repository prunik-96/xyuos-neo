#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>

#define EXT2_MAX_NAME 255

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFREG 0x8000

int ext2_mount(void);
int ext2_lookup(const char *path, uint32_t *out_inode_num, ext2_inode_t *out_inode);
uint32_t ext2_read(const ext2_inode_t *inode, uint32_t offset, void *buf, uint32_t len);

// Calls cb(name, name_len, is_dir, userdata) for every entry in the given
// directory inode. Returns the number of entries visited.
typedef void (*ext2_dirent_cb)(const char *name, uint8_t name_len, int is_dir, void *userdata);
int ext2_iterate_dir(const ext2_inode_t *dir_inode, ext2_dirent_cb cb, void *userdata);

// --- write support ---

// Load an inode by number (public wrapper around the internal reader).
void ext2_get_inode(uint32_t inode_num, ext2_inode_t *out);

// Write len bytes at offset into the file inode, growing/allocating blocks as
// needed (direct + single-indirect). Returns bytes written, or -1 on error.
int ext2_write(uint32_t inode_num, uint32_t offset, const void *buf, uint32_t len);

// Shrink or extend a regular file to new_size, freeing blocks past the new end
// and keeping i_blocks in step. Growing only records the size (leaves a hole).
// Returns 1 on success, 0 on failure (not mounted, or a directory).
int ext2_truncate(uint32_t inode_num, uint32_t new_size);

// Create a regular file (is_dir=0) or directory (is_dir=1) named `name` in the
// directory `parent_inode_num`. On success stores the new inode number in
// *out_inode_num and returns 1; returns 0 on failure (incl. name exists).
int ext2_create(uint32_t parent_inode_num, const char *name, int is_dir, uint32_t *out_inode_num);

// Remove `name` (a regular file or empty directory) from `parent_inode_num`,
// freeing its inode and data blocks. Returns 1 on success, 0 on failure.
int ext2_unlink(uint32_t parent_inode_num, const char *name);

// Relink an entry from old_parent/old_name to new_parent/new_name without
// touching the inode's data (rename/move). Fixes ".." and link counts when a
// directory moves between parents. Returns 1 on success, 0 on failure
// (source missing, or destination name already exists).
int ext2_move(uint32_t old_parent, const char *old_name,
              uint32_t new_parent, const char *new_name);

#endif
