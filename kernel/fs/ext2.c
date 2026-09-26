#include "ext2.h"
#include "../drivers/blkdev.h"
#include "../drivers/rtc.h"
#include "../kernel/kio.h"
#include <stddef.h>

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
} __attribute__((packed)) ext2_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_bgd_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} ext2_dirent_hdr_t;

#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INODE 2
#define MAX_BLOCK_SIZE 4096

static ext2_superblock_t sb;
static uint32_t block_size;
static uint32_t inode_size;
static uint32_t bgd_block;
static uint8_t block_buf[MAX_BLOCK_SIZE];
static uint8_t indirect_buf[MAX_BLOCK_SIZE];  // a single-indirect / level-2 table
static uint8_t dind_buf[MAX_BLOCK_SIZE];      // a double-indirect / level-1 table
static uint8_t wb_buf[MAX_BLOCK_SIZE];   // inode-table / zeroing read-modify-write
static uint8_t bmp_buf[MAX_BLOCK_SIZE];  // block/inode bitmaps
static uint8_t gd_buf[MAX_BLOCK_SIZE];   // group descriptor blocks
static int mounted = 0;

// The read path's own copies of the last indirect tables it looked at, and
// which blocks they are (0: none). Reading a file front to back asks for the
// same table 256 times running; without these, every kilobyte past the first
// 268 cost two table reads before its own. With fonts on the disk that is
// the common case -- the CJK face alone is sixteen thousand blocks.
//
// Separate from indirect_buf/dind_buf, which the write path fills and
// modifies as it pleases. A write to a block drops any copy of it here
// (write_block), so a copy is never older than the disk.
static uint8_t  rc_ind[MAX_BLOCK_SIZE], rc_dind[MAX_BLOCK_SIZE];
static uint32_t rc_ind_blk, rc_dind_blk;

// One block, in one request to the device: `out` is always one of the static
// buffers above, contiguous in physical memory as a device transfer needs.
static void read_block(uint32_t block_num, uint8_t *out) {
    uint32_t sectors_per_block = block_size / 512;
    blkdev_read_sectors((uint64_t)block_num * sectors_per_block,
                        sectors_per_block, out);
}

static const uint32_t *table(uint32_t blk, uint8_t *buf, uint32_t *tag) {
    if (*tag != blk) {
        read_block(blk, buf);
        *tag = blk;
    }
    return (const uint32_t *)buf;
}

int ext2_mount(void) {
    uint8_t sb_buf[1024];
    blkdev_read_sector(2, sb_buf);
    blkdev_read_sector(3, sb_buf + 512);

    for (uint32_t i = 0; i < sizeof(ext2_superblock_t); i++) {
        ((uint8_t *)&sb)[i] = sb_buf[i];
    }

    if (sb.s_magic != EXT2_MAGIC) {
        kprintf("ext2: bad magic 0x%x\n", sb.s_magic);
        return 0;
    }

    block_size = 1024u << sb.s_log_block_size;
    if (block_size > MAX_BLOCK_SIZE) {
        kprintf("ext2: block size %u unsupported\n", block_size);
        return 0;
    }
    inode_size = (sb.s_rev_level >= 1 && sb.s_inode_size != 0) ? sb.s_inode_size : 128;
    bgd_block = sb.s_first_data_block + 1;

    kprintf("ext2: mounted, block_size=%u inodes=%u\n", block_size, sb.s_inodes_count);
    rc_ind_blk = rc_dind_blk = 0;
    mounted = 1;
    return 1;
}

static void get_inode(uint32_t inode_num, ext2_inode_t *out) {
    uint32_t group = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index_in_group = (inode_num - 1) % sb.s_inodes_per_group;

    uint32_t bgd_per_block = block_size / sizeof(ext2_bgd_t);
    uint32_t bgd_blk = bgd_block + group / bgd_per_block;
    read_block(bgd_blk, block_buf);
    ext2_bgd_t *bgd = (ext2_bgd_t *)(block_buf) + (group % bgd_per_block);

    uint64_t byte_offset = (uint64_t)index_in_group * inode_size;
    uint32_t blk = bgd->bg_inode_table + (uint32_t)(byte_offset / block_size);
    uint32_t off_in_blk = (uint32_t)(byte_offset % block_size);

    read_block(blk, block_buf);
    uint32_t copy_size = sizeof(ext2_inode_t);
    if (copy_size > inode_size) copy_size = inode_size;
    for (uint32_t i = 0; i < copy_size; i++) {
        ((uint8_t *)out)[i] = block_buf[off_in_blk + i];
    }
}

static uint32_t resolve_block(const ext2_inode_t *inode, uint32_t block_index) {
    uint32_t per = block_size / 4;

    if (block_index < 12) {
        return inode->i_block[block_index];
    }
    block_index -= 12;

    // single indirect: i_block[12] -> per data blocks
    if (block_index < per) {
        if (inode->i_block[12] == 0) return 0;
        return table(inode->i_block[12], rc_ind, &rc_ind_blk)[block_index];
    }
    block_index -= per;

    // double indirect: i_block[13] -> per tables -> per data blocks each
    if (block_index < per * per) {
        if (inode->i_block[13] == 0) return 0;
        uint32_t outer = block_index / per;
        uint32_t inner = block_index % per;
        uint32_t l2 = table(inode->i_block[13], rc_dind, &rc_dind_blk)[outer];
        if (l2 == 0) return 0;
        return table(l2, rc_ind, &rc_ind_blk)[inner];
    }

    // triple indirect would start here; unreachable on a disk this small
    // (12 + per + per*per blocks is already 64 MiB at a 1 KiB block size).
    return 0;
}

uint32_t ext2_read(const ext2_inode_t *inode, uint32_t offset, void *buf, uint32_t len) {
    if (offset >= inode->i_size) return 0;
    if (offset + len > inode->i_size) len = inode->i_size - offset;

    uint8_t *out = (uint8_t *)buf;
    uint32_t total = 0;
    while (total < len) {
        uint32_t file_pos = offset + total;
        uint32_t block_index = file_pos / block_size;
        uint32_t block_off = file_pos % block_size;
        uint32_t blk = resolve_block(inode, block_index);

        uint32_t chunk = block_size - block_off;
        if (chunk > len - total) chunk = len - total;

        if (blk == 0) {
            for (uint32_t i = 0; i < chunk; i++) out[total + i] = 0;
        } else {
            read_block(blk, block_buf);
            for (uint32_t i = 0; i < chunk; i++) out[total + i] = block_buf[block_off + i];
        }
        total += chunk;
    }
    return total;
}

int ext2_iterate_dir(const ext2_inode_t *dir_inode, ext2_dirent_cb cb, void *userdata) {
    if ((dir_inode->i_mode & 0xF000) != EXT2_S_IFDIR) return 0;

    int count = 0;
    uint32_t num_blocks = (dir_inode->i_size + block_size - 1) / block_size;
    for (uint32_t bi = 0; bi < num_blocks; bi++) {
        uint32_t blk = resolve_block(dir_inode, bi);
        if (blk == 0) continue;
        read_block(blk, block_buf);

        uint32_t pos = 0;
        while (pos < block_size) {
            ext2_dirent_hdr_t *de = (ext2_dirent_hdr_t *)(block_buf + pos);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len > 0) {
                const char *name = (const char *)(block_buf + pos + sizeof(ext2_dirent_hdr_t));
                cb(name, de->name_len, de->file_type == 2, userdata);
                count++;
            }
            pos += de->rec_len;
        }
    }
    return count;
}

int ext2_lookup(const char *path, uint32_t *out_inode_num, ext2_inode_t *out_inode) {
    if (!mounted) return 0;

    uint32_t current_inode_num = EXT2_ROOT_INODE;
    ext2_inode_t current_inode;
    get_inode(current_inode_num, &current_inode);

    if (path[0] == '/') path++;
    if (path[0] == '\0') {
        *out_inode_num = current_inode_num;
        *out_inode = current_inode;
        return 1;
    }

    char component[EXT2_MAX_NAME + 1];
    while (path[0] != '\0') {
        uint32_t clen = 0;
        while (path[clen] != '/' && path[clen] != '\0' && clen < EXT2_MAX_NAME) {
            component[clen] = path[clen];
            clen++;
        }
        component[clen] = '\0';

        uint32_t num_blocks = (current_inode.i_size + block_size - 1) / block_size;
        uint32_t next_inode_num = 0;
        for (uint32_t bi = 0; bi < num_blocks && next_inode_num == 0; bi++) {
            uint32_t blk = resolve_block(&current_inode, bi);
            if (blk == 0) continue;
            read_block(blk, block_buf);

            uint32_t pos = 0;
            while (pos < block_size) {
                ext2_dirent_hdr_t *de = (ext2_dirent_hdr_t *)(block_buf + pos);
                if (de->rec_len == 0) break;
                if (de->inode != 0 && de->name_len == clen) {
                    const char *name = (const char *)(block_buf + pos + sizeof(ext2_dirent_hdr_t));
                    int match = 1;
                    for (uint32_t i = 0; i < clen; i++) {
                        if (name[i] != component[i]) { match = 0; break; }
                    }
                    if (match) {
                        next_inode_num = de->inode;
                        break;
                    }
                }
                pos += de->rec_len;
            }
        }

        if (next_inode_num == 0) return 0;
        current_inode_num = next_inode_num;
        get_inode(current_inode_num, &current_inode);

        path += clen;
        if (path[0] == '/') path++;
    }

    *out_inode_num = current_inode_num;
    *out_inode = current_inode;
    return 1;
}

// =========================================================================
//  Write support
// =========================================================================

static void write_block(uint32_t block_num, const uint8_t *in) {
    if (block_num == rc_ind_blk)  rc_ind_blk = 0;    // see read_block
    if (block_num == rc_dind_blk) rc_dind_blk = 0;
    uint32_t sectors_per_block = block_size / 512;
    uint64_t first_lba = (uint64_t)block_num * sectors_per_block;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        blkdev_write_sector(first_lba + i, in + i * 512);
    }
}

static uint32_t num_groups(void) {
    return (sb.s_blocks_count - sb.s_first_data_block + sb.s_blocks_per_group - 1)
           / sb.s_blocks_per_group;
}

// The primary superblock always lives at byte offset 1024 (LBA 2-3). We only
// ever change the free counts, so read-modify-write those two fields in place.
static void sb_writeback(void) {
    uint8_t buf[1024];
    blkdev_read_sector(2, buf);
    blkdev_read_sector(3, buf + 512);
    *(uint32_t *)(buf + 12) = sb.s_free_blocks_count;  // s_free_blocks_count
    *(uint32_t *)(buf + 16) = sb.s_free_inodes_count;  // s_free_inodes_count
    blkdev_write_sector(2, buf);
    blkdev_write_sector(3, buf + 512);
}

static void bgd_get(uint32_t group, ext2_bgd_t *out) {
    uint32_t per = block_size / sizeof(ext2_bgd_t);
    uint32_t blk = bgd_block + group / per;
    read_block(blk, gd_buf);
    *out = *((ext2_bgd_t *)gd_buf + (group % per));
}

static void bgd_writeback(uint32_t group, const ext2_bgd_t *in) {
    uint32_t per = block_size / sizeof(ext2_bgd_t);
    uint32_t blk = bgd_block + group / per;
    read_block(blk, gd_buf);
    *((ext2_bgd_t *)gd_buf + (group % per)) = *in;
    write_block(blk, gd_buf);
}

static void put_inode(uint32_t inode_num, const ext2_inode_t *in) {
    uint32_t group = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index_in_group = (inode_num - 1) % sb.s_inodes_per_group;

    ext2_bgd_t bgd;
    bgd_get(group, &bgd);

    uint64_t byte_offset = (uint64_t)index_in_group * inode_size;
    uint32_t blk = bgd.bg_inode_table + (uint32_t)(byte_offset / block_size);
    uint32_t off_in_blk = (uint32_t)(byte_offset % block_size);

    read_block(blk, wb_buf);
    uint32_t copy_size = sizeof(ext2_inode_t);
    if (copy_size > inode_size) copy_size = inode_size;
    for (uint32_t i = 0; i < copy_size; i++) {
        wb_buf[off_in_blk + i] = ((const uint8_t *)in)[i];
    }
    write_block(blk, wb_buf);
}

static void zero_block(uint32_t blk) {
    for (uint32_t i = 0; i < block_size; i++) wb_buf[i] = 0;
    write_block(blk, wb_buf);
}

static uint32_t alloc_block(void) {
    uint32_t groups = num_groups();
    for (uint32_t g = 0; g < groups; g++) {
        ext2_bgd_t bgd;
        bgd_get(g, &bgd);
        if (bgd.bg_free_blocks_count == 0) continue;
        read_block(bgd.bg_block_bitmap, bmp_buf);
        for (uint32_t i = 0; i < sb.s_blocks_per_group; i++) {
            if (!(bmp_buf[i / 8] & (1 << (i % 8)))) {
                bmp_buf[i / 8] |= (1 << (i % 8));
                write_block(bgd.bg_block_bitmap, bmp_buf);
                bgd.bg_free_blocks_count--;
                bgd_writeback(g, &bgd);
                sb.s_free_blocks_count--;
                sb_writeback();
                return sb.s_first_data_block + g * sb.s_blocks_per_group + i;
            }
        }
    }
    return 0;
}

static void free_block(uint32_t blk) {
    uint32_t rel = blk - sb.s_first_data_block;
    uint32_t g = rel / sb.s_blocks_per_group;
    uint32_t i = rel % sb.s_blocks_per_group;
    ext2_bgd_t bgd;
    bgd_get(g, &bgd);
    read_block(bgd.bg_block_bitmap, bmp_buf);
    bmp_buf[i / 8] &= ~(1 << (i % 8));
    write_block(bgd.bg_block_bitmap, bmp_buf);
    bgd.bg_free_blocks_count++;
    bgd_writeback(g, &bgd);
    sb.s_free_blocks_count++;
    sb_writeback();
}

static uint32_t alloc_inode(void) {
    uint32_t groups = num_groups();
    for (uint32_t g = 0; g < groups; g++) {
        ext2_bgd_t bgd;
        bgd_get(g, &bgd);
        if (bgd.bg_free_inodes_count == 0) continue;
        read_block(bgd.bg_inode_bitmap, bmp_buf);
        for (uint32_t i = 0; i < sb.s_inodes_per_group; i++) {
            // reserved inodes are already marked used in the bitmap by mke2fs,
            // so a plain first-free scan naturally skips them.
            if (!(bmp_buf[i / 8] & (1 << (i % 8)))) {
                bmp_buf[i / 8] |= (1 << (i % 8));
                write_block(bgd.bg_inode_bitmap, bmp_buf);
                bgd.bg_free_inodes_count--;
                bgd_writeback(g, &bgd);
                sb.s_free_inodes_count--;
                sb_writeback();
                return g * sb.s_inodes_per_group + i + 1;
            }
        }
    }
    return 0;
}

static void free_inode(uint32_t ino) {
    uint32_t g = (ino - 1) / sb.s_inodes_per_group;
    uint32_t i = (ino - 1) % sb.s_inodes_per_group;
    ext2_bgd_t bgd;
    bgd_get(g, &bgd);
    read_block(bgd.bg_inode_bitmap, bmp_buf);
    bmp_buf[i / 8] &= ~(1 << (i % 8));
    write_block(bgd.bg_inode_bitmap, bmp_buf);
    bgd.bg_free_inodes_count++;
    bgd_writeback(g, &bgd);
    sb.s_free_inodes_count++;
    sb_writeback();
}

// Keep bg_used_dirs_count in sync so fsck's group summary matches. delta is
// +1 when a directory is created in the inode's group, -1 when removed.
static void adjust_used_dirs(uint32_t inode_num, int delta) {
    uint32_t g = (inode_num - 1) / sb.s_inodes_per_group;
    ext2_bgd_t bgd;
    bgd_get(g, &bgd);
    if (delta < 0 && bgd.bg_used_dirs_count > 0) bgd.bg_used_dirs_count--;
    else if (delta > 0) bgd.bg_used_dirs_count++;
    bgd_writeback(g, &bgd);
}

// Wipe an inode-table entry on delete (all zero: links=0, no blocks) so fsck
// doesn't see a zombie inode whose bitmap bit is free but whose table entry
// still looks live. We leave i_dtime at 0 rather than a fake timestamp: a
// nonzero i_dtime makes fsck try to interpret the inode as an orphan-list node.
static void clear_inode_entry(uint32_t inode_num) {
    ext2_inode_t empty;
    for (uint32_t i = 0; i < sizeof(empty); i++) ((uint8_t *)&empty)[i] = 0;
    put_inode(inode_num, &empty);
}

// Returns the physical block backing block_index of `inode`, allocating it (and
// the single-indirect block if needed) on demand. Sets *dirty when the inode's
// i_block/i_blocks changed and must be written back by the caller.
static uint32_t get_or_alloc_block(ext2_inode_t *inode, uint32_t block_index, int *dirty) {
    uint32_t spb = block_size / 512;
    if (block_index < 12) {
        if (inode->i_block[block_index] == 0) {
            uint32_t blk = alloc_block();
            if (blk == 0) return 0;
            zero_block(blk);
            inode->i_block[block_index] = blk;
            inode->i_blocks += spb;
            *dirty = 1;
        }
        return inode->i_block[block_index];
    }

    uint32_t per = block_size / 4;
    uint32_t idx = block_index - 12;

    // single indirect
    if (idx < per) {
        if (inode->i_block[12] == 0) {
            uint32_t ind = alloc_block();
            if (ind == 0) return 0;
            zero_block(ind);
            inode->i_block[12] = ind;
            inode->i_blocks += spb;
            *dirty = 1;
        }
        read_block(inode->i_block[12], indirect_buf);
        uint32_t *ptrs = (uint32_t *)indirect_buf;
        if (ptrs[idx] == 0) {
            uint32_t blk = alloc_block();
            if (blk == 0) return 0;
            zero_block(blk);
            ptrs[idx] = blk;
            write_block(inode->i_block[12], indirect_buf);
            inode->i_blocks += spb;
            *dirty = 1;
        }
        return ptrs[idx];
    }
    idx -= per;

    // double indirect. alloc_block/zero_block use bmp_buf/gd_buf/wb_buf, never
    // dind_buf or indirect_buf, so the two table images stay valid across the
    // allocations below.
    if (idx < per * per) {
        if (inode->i_block[13] == 0) {
            uint32_t t = alloc_block();
            if (t == 0) return 0;
            zero_block(t);
            inode->i_block[13] = t;
            inode->i_blocks += spb;
            *dirty = 1;
        }
        read_block(inode->i_block[13], dind_buf);       // level-1 table
        uint32_t *l1 = (uint32_t *)dind_buf;
        uint32_t outer = idx / per;
        uint32_t inner = idx % per;

        if (l1[outer] == 0) {
            uint32_t t = alloc_block();
            if (t == 0) return 0;
            zero_block(t);
            l1[outer] = t;
            write_block(inode->i_block[13], dind_buf);
            inode->i_blocks += spb;
            *dirty = 1;
        }
        read_block(l1[outer], indirect_buf);            // level-2 table
        uint32_t *l2 = (uint32_t *)indirect_buf;
        if (l2[inner] == 0) {
            uint32_t blk = alloc_block();
            if (blk == 0) return 0;
            zero_block(blk);
            l2[inner] = blk;
            write_block(l1[outer], indirect_buf);
            inode->i_blocks += spb;
            *dirty = 1;
        }
        return l2[inner];
    }

    return 0; // beyond double-indirect: unsupported (unreachable on this disk)
}

void ext2_get_inode(uint32_t inode_num, ext2_inode_t *out) {
    get_inode(inode_num, out);
}

int ext2_write(uint32_t inode_num, uint32_t offset, const void *buf, uint32_t len) {
    if (!mounted) return -1;
    ext2_inode_t inode;
    get_inode(inode_num, &inode);

    const uint8_t *in = (const uint8_t *)buf;
    uint32_t total = 0;
    int dirty = 0;

    while (total < len) {
        uint32_t pos = offset + total;
        uint32_t block_index = pos / block_size;
        uint32_t block_off = pos % block_size;
        uint32_t blk = get_or_alloc_block(&inode, block_index, &dirty);
        if (blk == 0) break;

        uint32_t chunk = block_size - block_off;
        if (chunk > len - total) chunk = len - total;

        read_block(blk, block_buf);
        for (uint32_t i = 0; i < chunk; i++) block_buf[block_off + i] = in[total + i];
        write_block(blk, block_buf);
        total += chunk;
    }

    if (offset + total > inode.i_size) {
        inode.i_size = offset + total;
        dirty = 1;
    }
    if (total > 0) {                        // record the modification time
        inode.i_mtime = inode.i_ctime = (uint32_t)rtc_now_unix();
        dirty = 1;
    }
    if (dirty) put_inode(inode_num, &inode);
    return (int)total;
}

static uint32_t dirent_reclen(uint8_t name_len) {
    return (8u + name_len + 3u) & ~3u;
}

static int name_matches(const uint8_t *entry_name, uint8_t entry_len,
                        const char *name, uint8_t name_len) {
    if (entry_len != name_len) return 0;
    for (uint8_t i = 0; i < name_len; i++) {
        if (entry_name[i] != (uint8_t)name[i]) return 0;
    }
    return 1;
}

static int dir_has_name(uint32_t dir_inode_num, const char *name, uint8_t name_len,
                        uint32_t *out_ino) {
    ext2_inode_t dir;
    get_inode(dir_inode_num, &dir);
    uint32_t num_blocks = dir.i_size / block_size;
    for (uint32_t bi = 0; bi < num_blocks; bi++) {
        uint32_t blk = resolve_block(&dir, bi);
        if (blk == 0) continue;
        read_block(blk, block_buf);
        uint32_t pos = 0;
        while (pos < block_size) {
            ext2_dirent_hdr_t *de = (ext2_dirent_hdr_t *)(block_buf + pos);
            if (de->rec_len == 0) break;
            if (de->inode != 0 &&
                name_matches(block_buf + pos + 8, de->name_len, name, name_len)) {
                if (out_ino) *out_ino = de->inode;
                return 1;
            }
            pos += de->rec_len;
        }
    }
    return 0;
}

static int add_dirent(uint32_t dir_inode_num, const char *name, uint8_t name_len,
                      uint32_t child_ino, uint8_t file_type) {
    ext2_inode_t dir;
    get_inode(dir_inode_num, &dir);
    uint32_t need = dirent_reclen(name_len);
    uint32_t num_blocks = dir.i_size / block_size;

    for (uint32_t bi = 0; bi < num_blocks; bi++) {
        uint32_t blk = resolve_block(&dir, bi);
        if (blk == 0) continue;
        read_block(blk, block_buf);
        uint32_t pos = 0;
        while (pos < block_size) {
            ext2_dirent_hdr_t *de = (ext2_dirent_hdr_t *)(block_buf + pos);
            if (de->rec_len == 0) break;
            uint32_t used = (de->inode == 0) ? 0 : dirent_reclen(de->name_len);
            if (de->rec_len >= used + need) {
                uint32_t old_rec = de->rec_len;
                if (de->inode == 0) {
                    de->inode = child_ino;
                    de->name_len = name_len;
                    de->file_type = file_type;
                    de->rec_len = old_rec;
                    for (uint8_t i = 0; i < name_len; i++) block_buf[pos + 8 + i] = name[i];
                } else {
                    de->rec_len = used;
                    ext2_dirent_hdr_t *nd = (ext2_dirent_hdr_t *)(block_buf + pos + used);
                    nd->inode = child_ino;
                    nd->name_len = name_len;
                    nd->file_type = file_type;
                    nd->rec_len = old_rec - used;
                    for (uint8_t i = 0; i < name_len; i++) block_buf[pos + used + 8 + i] = name[i];
                }
                write_block(blk, block_buf);
                return 1;
            }
            pos += de->rec_len;
        }
    }

    // No room in existing blocks: append a fresh directory block.
    int dirty = 0;
    uint32_t bi = dir.i_size / block_size;
    uint32_t blk = get_or_alloc_block(&dir, bi, &dirty);
    if (blk == 0) return 0;
    for (uint32_t i = 0; i < block_size; i++) block_buf[i] = 0;
    ext2_dirent_hdr_t *de = (ext2_dirent_hdr_t *)block_buf;
    de->inode = child_ino;
    de->name_len = name_len;
    de->file_type = file_type;
    de->rec_len = block_size;
    for (uint8_t i = 0; i < name_len; i++) block_buf[8 + i] = name[i];
    write_block(blk, block_buf);
    dir.i_size += block_size;
    put_inode(dir_inode_num, &dir);
    return 1;
}

static int remove_dirent(uint32_t dir_inode_num, const char *name, uint8_t name_len,
                         uint32_t *out_ino) {
    ext2_inode_t dir;
    get_inode(dir_inode_num, &dir);
    uint32_t num_blocks = dir.i_size / block_size;
    for (uint32_t bi = 0; bi < num_blocks; bi++) {
        uint32_t blk = resolve_block(&dir, bi);
        if (blk == 0) continue;
        read_block(blk, block_buf);
        uint32_t pos = 0, prev = 0;
        int first = 1;
        while (pos < block_size) {
            ext2_dirent_hdr_t *de = (ext2_dirent_hdr_t *)(block_buf + pos);
            if (de->rec_len == 0) break;
            if (de->inode != 0 &&
                name_matches(block_buf + pos + 8, de->name_len, name, name_len)) {
                if (out_ino) *out_ino = de->inode;
                if (first) {
                    de->inode = 0; // keep rec_len -> becomes an empty slot
                } else {
                    ext2_dirent_hdr_t *pd = (ext2_dirent_hdr_t *)(block_buf + prev);
                    pd->rec_len += de->rec_len;
                }
                write_block(blk, block_buf);
                return 1;
            }
            prev = pos;
            first = 0;
            pos += de->rec_len;
        }
    }
    return 0;
}

static void free_inode_data(const ext2_inode_t *inode) {
    uint32_t per = block_size / 4;

    // Data blocks first (resolve_block walks direct + single + double indirect).
    // free_block uses bmp_buf/gd_buf, so resolve_block's dind_buf/indirect_buf
    // images are not disturbed between calls.
    uint32_t num_blocks = (inode->i_size + block_size - 1) / block_size;
    for (uint32_t bi = 0; bi < num_blocks; bi++) {
        uint32_t blk = resolve_block(inode, bi);
        if (blk) free_block(blk);
    }

    // Then the pointer tables themselves.
    if (inode->i_block[12]) free_block(inode->i_block[12]);

    if (inode->i_block[13]) {
        read_block(inode->i_block[13], dind_buf);
        uint32_t *l1 = (uint32_t *)dind_buf;
        for (uint32_t o = 0; o < per; o++) {
            if (l1[o]) free_block(l1[o]);   // a level-2 table
        }
        free_block(inode->i_block[13]);     // the level-1 table
    }
}

// Shrink (or extend) a regular file to new_size, freeing any blocks that fall
// entirely past the new end. Growing only records the larger size -- the gap is
// left unallocated (a hole), matching how ext2_write already behaves when a
// caller seeks past the end.
//
// i_blocks is decremented for every freed block: get_or_alloc_block increments
// it on the way up, so failing to mirror that here makes e2fsck report a wrong
// block count.
int ext2_truncate(uint32_t inode_num, uint32_t new_size) {
    if (!mounted) return 0;

    ext2_inode_t inode;
    ext2_get_inode(inode_num, &inode);
    if ((inode.i_mode & 0xF000) == EXT2_S_IFDIR) return 0;   // never truncate a directory
    if (new_size == inode.i_size) return 1;

    if (new_size > inode.i_size) {
        inode.i_size = new_size;
        put_inode(inode_num, &inode);
        return 1;
    }

    uint32_t spb        = block_size / 512;
    uint32_t old_blocks = (inode.i_size + block_size - 1) / block_size;
    uint32_t new_blocks = (new_size + block_size - 1) / block_size;
    uint32_t per        = block_size / 4;

    // direct blocks
    for (uint32_t bi = new_blocks; bi < old_blocks && bi < 12; bi++) {
        if (inode.i_block[bi]) {
            free_block(inode.i_block[bi]);
            inode.i_block[bi] = 0;
            if (inode.i_blocks >= spb) inode.i_blocks -= spb;
        }
    }

    // single-indirect blocks. free_block() works out of bmp_buf/gd_buf, so the
    // pointer table can safely stay in indirect_buf across these calls.
    if (old_blocks > 12 && inode.i_block[12]) {
        read_block(inode.i_block[12], indirect_buf);
        uint32_t *ptrs = (uint32_t *)indirect_buf;
        uint32_t start = (new_blocks > 12) ? (new_blocks - 12) : 0;
        uint32_t end   = old_blocks - 12;
        if (end > per) end = per;
        int changed = 0;
        for (uint32_t i = start; i < end; i++) {
            if (ptrs[i]) {
                free_block(ptrs[i]);
                ptrs[i] = 0;
                changed = 1;
                if (inode.i_blocks >= spb) inode.i_blocks -= spb;
            }
        }
        if (changed) write_block(inode.i_block[12], indirect_buf);

        // the indirect block itself is only needed once the file reaches it
        if (new_blocks <= 12) {
            free_block(inode.i_block[12]);
            inode.i_block[12] = 0;
            if (inode.i_blocks >= spb) inode.i_blocks -= spb;
        }
    }

    // double-indirect blocks. Same shape, one level deeper: for every level-2
    // table, free the data entries that fall past the new end, then free the
    // table itself once nothing in it survives, and finally the level-1 table
    // once the file no longer reaches double-indirect at all.
    uint32_t di_base = 12 + per;
    if (old_blocks > di_base && inode.i_block[13]) {
        read_block(inode.i_block[13], dind_buf);
        uint32_t *l1 = (uint32_t *)dind_buf;
        uint32_t keep = (new_blocks > di_base) ? (new_blocks - di_base) : 0;
        uint32_t total_di = old_blocks - di_base;
        int l1_changed = 0;

        for (uint32_t o = 0; o < per; o++) {
            if (l1[o] == 0) continue;
            read_block(l1[o], indirect_buf);
            uint32_t *l2 = (uint32_t *)indirect_buf;
            int l2_changed = 0, any_left = 0;

            for (uint32_t inner = 0; inner < per; inner++) {
                uint32_t idx = o * per + inner;
                if (idx >= total_di) break;      // never existed
                if (idx < keep) {                // survives
                    if (l2[inner]) any_left = 1;
                    continue;
                }
                if (l2[inner]) {
                    free_block(l2[inner]);
                    l2[inner] = 0;
                    l2_changed = 1;
                    if (inode.i_blocks >= spb) inode.i_blocks -= spb;
                }
            }

            if (any_left) {
                if (l2_changed) write_block(l1[o], indirect_buf);
            } else {
                free_block(l1[o]);               // whole level-2 table now empty
                l1[o] = 0;
                l1_changed = 1;
                if (inode.i_blocks >= spb) inode.i_blocks -= spb;
            }
        }

        if (l1_changed) write_block(inode.i_block[13], dind_buf);

        if (new_blocks <= di_base) {
            free_block(inode.i_block[13]);
            inode.i_block[13] = 0;
            if (inode.i_blocks >= spb) inode.i_blocks -= spb;
        }
    }

    inode.i_size = new_size;
    put_inode(inode_num, &inode);
    return 1;
}

int ext2_create(uint32_t parent_inode_num, const char *name, int is_dir,
                uint32_t *out_inode_num) {
    if (!mounted) return 0;
    uint8_t nlen = 0;
    while (name[nlen]) nlen++;
    if (nlen == 0 || nlen > EXT2_MAX_NAME) return 0;
    if (dir_has_name(parent_inode_num, name, nlen, NULL)) return 0;

    uint32_t ino = alloc_inode();
    if (ino == 0) return 0;

    ext2_inode_t ni;
    for (uint32_t i = 0; i < sizeof(ni); i++) ((uint8_t *)&ni)[i] = 0;
    ni.i_mode = is_dir ? (EXT2_S_IFDIR | 0755) : (EXT2_S_IFREG | 0644);
    ni.i_links_count = is_dir ? 2 : 1;
    ni.i_size = 0;
    ni.i_blocks = 0;
    ni.i_atime = ni.i_ctime = ni.i_mtime = (uint32_t)rtc_now_unix();

    if (is_dir) {
        int dirty = 0;
        uint32_t blk = get_or_alloc_block(&ni, 0, &dirty);
        if (blk == 0) { free_inode(ino); return 0; }
        for (uint32_t i = 0; i < block_size; i++) block_buf[i] = 0;
        ext2_dirent_hdr_t *d1 = (ext2_dirent_hdr_t *)block_buf;
        d1->inode = ino; d1->name_len = 1; d1->file_type = 2; d1->rec_len = dirent_reclen(1);
        block_buf[8] = '.';
        ext2_dirent_hdr_t *d2 = (ext2_dirent_hdr_t *)(block_buf + d1->rec_len);
        d2->inode = parent_inode_num; d2->name_len = 2; d2->file_type = 2;
        d2->rec_len = block_size - d1->rec_len;
        block_buf[d1->rec_len + 8] = '.';
        block_buf[d1->rec_len + 9] = '.';
        write_block(blk, block_buf);
        ni.i_size = block_size;

        ext2_inode_t parent;
        get_inode(parent_inode_num, &parent);
        parent.i_links_count++;
        put_inode(parent_inode_num, &parent);
    }

    put_inode(ino, &ni);

    if (is_dir) adjust_used_dirs(ino, +1);

    if (!add_dirent(parent_inode_num, name, nlen, ino, is_dir ? 2 : 1)) {
        if (is_dir) adjust_used_dirs(ino, -1);
        free_inode_data(&ni);
        free_inode(ino);
        return 0;
    }
    if (out_inode_num) *out_inode_num = ino;
    return 1;
}

int ext2_unlink(uint32_t parent_inode_num, const char *name) {
    if (!mounted) return 0;
    uint8_t nlen = 0;
    while (name[nlen]) nlen++;
    if (nlen == 0) return 0;

    uint32_t child = 0;
    if (!dir_has_name(parent_inode_num, name, nlen, &child)) return 0;

    ext2_inode_t ci;
    get_inode(child, &ci);
    int is_dir = ((ci.i_mode & 0xF000) == EXT2_S_IFDIR);

    if (is_dir) {
        // Only allow removing an empty directory (nothing beyond "." and "..").
        int entries = 0;
        uint32_t num_blocks = ci.i_size / block_size;
        for (uint32_t bi = 0; bi < num_blocks; bi++) {
            uint32_t blk = resolve_block(&ci, bi);
            if (blk == 0) continue;
            read_block(blk, block_buf);
            uint32_t pos = 0;
            while (pos < block_size) {
                ext2_dirent_hdr_t *de = (ext2_dirent_hdr_t *)(block_buf + pos);
                if (de->rec_len == 0) break;
                if (de->inode != 0) {
                    const uint8_t *nm = block_buf + pos + 8;
                    int is_dot = (de->name_len == 1 && nm[0] == '.');
                    int is_dotdot = (de->name_len == 2 && nm[0] == '.' && nm[1] == '.');
                    if (!is_dot && !is_dotdot) entries++;
                }
                pos += de->rec_len;
            }
        }
        if (entries > 0) return 0;
    }

    uint32_t removed = 0;
    if (!remove_dirent(parent_inode_num, name, nlen, &removed)) return 0;

    get_inode(child, &ci);
    free_inode_data(&ci);
    clear_inode_entry(child);
    free_inode(child);
    if (is_dir) adjust_used_dirs(child, -1);

    if (is_dir) {
        ext2_inode_t parent;
        get_inode(parent_inode_num, &parent);
        if (parent.i_links_count > 0) parent.i_links_count--;
        put_inode(parent_inode_num, &parent);
    }
    return 1;
}

int ext2_move(uint32_t old_parent, const char *old_name,
              uint32_t new_parent, const char *new_name) {
    if (!mounted) return 0;
    uint8_t oldlen = 0; while (old_name[oldlen]) oldlen++;
    uint8_t newlen = 0; while (new_name[newlen]) newlen++;
    if (oldlen == 0 || newlen == 0 || newlen > EXT2_MAX_NAME) return 0;

    uint32_t child = 0;
    if (!dir_has_name(old_parent, old_name, oldlen, &child)) return 0;
    if (dir_has_name(new_parent, new_name, newlen, NULL)) return 0; // dest exists

    ext2_inode_t ci;
    get_inode(child, &ci);
    int is_dir = ((ci.i_mode & 0xF000) == EXT2_S_IFDIR);

    if (!add_dirent(new_parent, new_name, newlen, child, is_dir ? 2 : 1)) return 0;
    uint32_t removed = 0;
    remove_dirent(old_parent, old_name, oldlen, &removed);

    if (is_dir && new_parent != old_parent) {
        // Repoint the child's ".." at its new parent and fix link counts.
        uint32_t blk = resolve_block(&ci, 0);
        if (blk) {
            read_block(blk, block_buf);
            ext2_dirent_hdr_t *d1 = (ext2_dirent_hdr_t *)block_buf;
            ext2_dirent_hdr_t *d2 = (ext2_dirent_hdr_t *)(block_buf + d1->rec_len);
            d2->inode = new_parent;
            write_block(blk, block_buf);
        }
        ext2_inode_t op;
        get_inode(old_parent, &op);
        if (op.i_links_count > 0) op.i_links_count--;
        put_inode(old_parent, &op);
        ext2_inode_t np;
        get_inode(new_parent, &np);
        np.i_links_count++;
        put_inode(new_parent, &np);
    }
    return 1;
}
