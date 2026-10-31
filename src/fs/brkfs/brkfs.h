#ifndef BRKFS_H
#define BRKFS_H

#include <brk/blkdev.h>
#include <brk/fs_types.h>
#include <brk/pagecache.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <uapi/types.h>

/*
 * brkfs on-disk format.
 *
 * Keep this layout in sync with the copy in the host tools
 * (brk-tools/brkfs.h).  The host tools cannot include this header directly
 * because it also contains kernel-only declarations.
 */

#define BRKFS_DIRECT_BLOCKS 7 /* Total number of direct block pointers */
#define BRKFS_INDIRECT_BLOCK \
	BRKFS_DIRECT_BLOCKS /* Single indirect block pointer index */
#define BRKFS_DOUBLE_INDIRECT_BLOCK \
	(BRKFS_INDIRECT_BLOCK + 1) /* Double indirect block pointer index */
#define BRKFS_TRIPLE_INDIRECT_BLOCK    \
	(BRKFS_DOUBLE_INDIRECT_BLOCK + \
	 1) /* Triple indirect block pointer index */
#define BRKFS_BLOCKS \
	(BRKFS_TRIPLE_INDIRECT_BLOCK + 1) /* Total number of block pointers */

#define BRKFS_ROOT_INO 1
#define BRKFS_MAGIC 0x6b7262

#define BRKFS_SUPER_OFFSET 1024
#define BRKFS_SUPER_SIZE 1024
#define BRKFS_SUPER_END_OFFSET (BRKFS_SUPER_OFFSET + BRKFS_SUPER_SIZE)

#define BRKFS_DIR_ENTRY_MIN_LEN 12
#define BRKFS_DIR_CSUM_SIZE 4
#define BRKFS_NAME_LEN 255

#define BRKFS_FORMAT_VERSION 2

/* Superblock state values for format version 2 and later. */
#define BRKFS_STATE_CLEAN 1
#define BRKFS_STATE_DIRTY 2
#define BRKFS_STATE_ERROR 3

/*
 * Feature bits describe format capabilities that an image actually uses.
 * Version 0 images are legacy images with no feature bits set.
 */
#define BRKFS_FEATURE_DOUBLE_INDIRECT (1u << 0)
#define BRKFS_FEATURE_TRIPLE_INDIRECT (1u << 1)
#define BRKFS_FEATURE_DIR_INDIRECT (1u << 2)
#define BRKFS_FEATURE_ALL                                           \
	(BRKFS_FEATURE_DOUBLE_INDIRECT | BRKFS_FEATURE_TRIPLE_INDIRECT | \
	 BRKFS_FEATURE_DIR_INDIRECT)

#define BRKFS_MIN_BLOCK_SIZE 1024
#define BRKFS_MAX_BLOCK_SIZE 4096
#define BRKFS_MIN_INODE_SIZE 128
#define BRKFS_MAX_INODE_SIZE 256

struct brkfs_super_block {
	u32 s_blocksize; /* Block size */
	u32 s_inode_bitmap; /* Inode bitmap start block number */
	u32 s_inodes_count; /* Total number of inodes */
	u32 s_data_block_bitmap; /* Data block bitmap start block number */
	u32 s_data_blocks_count; /* Total number of data blocks */
	u32 s_inode_table; /* Inode table start block number */
	u32 s_first_data_block; /* First data block number */
	u32 s_magic; /* File system magic number */
	u32 s_inode_size; /* Size of an inode */
	u32 s_blocks_count; /* Total number of blocks */
	u32 s_version; /* On-disk format version */
	u32 s_features; /* Format feature bits used by this image */
	u32 s_state; /* BRKFS_STATE_* */
	u8 s_uuid[16]; /* Volume UUID */
	char s_label[64]; /* Volume label */
	u32 s_free_blocks; /* Free data blocks */
	u32 s_free_inodes; /* Free inode slots */
	u32 s_mtime; /* Last mount time */
	u32 s_lastcheck; /* Last fsck time */
	u32 s_mnt_count; /* Number of mounts since last check */
	u32 s_max_mnt_count; /* Mounts before a check is recommended */
	u32 s_backup_super; /* Backup superblock block number, 0 if none */
	u32 s_orphan; /* Head of the orphan inode list */
	u32 s_checksum; /* CRC32C over this superblock */
	u8 __s_padding[BRKFS_SUPER_SIZE - 168];
};

struct brkfs_inode {
	u32 i_ino;
	u32 i_mode;
	u32 i_rdev;
	u32 i_flags;
	u32 i_nlink;
	u32 i_size;
	u32 i_atime;
	u32 i_atime_nsec;
	u32 i_ctime;
	u32 i_ctime_nsec;
	u32 i_mtime;
	u32 i_mtime_nsec;
	u32 i_uid;
	u32 i_gid;
	u32 i_block[BRKFS_BLOCKS];
	u32 i_next_orphan;
	u32 i_checksum;
};

struct brkfs_dir_entry {
	u32 inode;
	u16 entry_len;
	u8 name_len;
	u8 file_type;
	char name[BRKFS_NAME_LEN];
};

_Static_assert(sizeof(struct brkfs_super_block) == BRKFS_SUPER_SIZE,
	       "brkfs_super_block must match BRKFS_SUPER_SIZE");
_Static_assert(sizeof(struct brkfs_inode) <= BRKFS_MAX_INODE_SIZE,
	       "brkfs_inode is larger than BRKFS_MAX_INODE_SIZE");

/* CRC32C (Castagnoli), reflected polynomial 0x82f63b78. */
static inline u32 brkfs_crc32c(u32 crc, const void *buf, u32 len)
{
	const u8 *p = buf;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
	}
	return ~crc;
}

static inline u32 brkfs_super_checksum(struct brkfs_super_block *sb)
{
	u32 saved = sb->s_checksum;
	u32 crc;

	sb->s_checksum = 0;
	crc = brkfs_crc32c(0, sb, sizeof(*sb));
	sb->s_checksum = saved;
	return crc;
}

static inline void brkfs_super_set_checksum(struct brkfs_super_block *sb)
{
	sb->s_checksum = 0;
	sb->s_checksum = brkfs_crc32c(0, sb, sizeof(*sb));
}

static inline void brkfs_inode_set_checksum(struct brkfs_inode *inode)
{
	inode->i_checksum = 0;
	inode->i_checksum = brkfs_crc32c(0, inode, sizeof(*inode));
}

static inline bool brkfs_inode_checksum_ok(const struct brkfs_inode *inode)
{
	struct brkfs_inode copy = *inode;

	copy.i_checksum = 0;
	return inode->i_checksum ==
	       brkfs_crc32c(0, &copy, sizeof(copy));
}

static inline void brkfs_dir_set_checksum(void *buf, u32 block_size)
{
	u32 crc = brkfs_crc32c(0, buf, block_size - BRKFS_DIR_CSUM_SIZE);

	*(u32 *)((u8 *)buf + block_size - BRKFS_DIR_CSUM_SIZE) = crc;
}

static inline bool brkfs_dir_checksum_ok(const void *buf, u32 block_size)
{
	u32 stored = *(const u32 *)((const u8 *)buf + block_size -
				     BRKFS_DIR_CSUM_SIZE);

	return stored ==
	       brkfs_crc32c(0, buf, block_size - BRKFS_DIR_CSUM_SIZE);
}

/* Features the current kernel implementation can mount. */
#define BRKFS_KERNEL_SUPPORTED_FEATURES BRKFS_FEATURE_ALL

struct brkfs_sb_info {
	struct block_dev *s_bdev;
	struct brkfs_super_block s_sb;
	u32 s_inodes_per_block;
	u32 s_bits_per_block;
	bool s_super_dirty;
	sleeplock_t s_orphan_lock;
	spinlock_t s_super_lock;
};

struct brkfs_inode_info {
	u32 i_block[BRKFS_BLOCKS];
	u32 i_next_orphan;
};

struct brkfs_block {
	struct cached_page *cp;
	void *data;
};

int brkfs_get_block(struct brkfs_sb_info *sbi, u32 bno, struct brkfs_block *bb);
void brkfs_put_block(struct brkfs_block *bb);

struct brkfs_sb_info *brkfs_sb_info_alloc(struct block_dev *bdev,
					  struct brkfs_super_block *sb);
void brkfs_sb_info_free(struct brkfs_sb_info *sbi);
int brkfs_super_set_features(struct brkfs_sb_info *sbi, u32 features);
int brkfs_super_write(struct brkfs_sb_info *sbi);
void brkfs_orphan_cleanup(struct brkfs_sb_info *sbi);
void brkfs_orphan_remove(struct brkfs_sb_info *sbi, u32 ino);
int brkfs_inode_read(struct brkfs_sb_info *sbi, struct fs_inode *inode);
void brkfs_inode_setup_ops(struct fs_inode *inode);
int brkfs_inode_write(struct brkfs_sb_info *sbi, struct fs_inode *inode);
int brkfs_inode_alloc(struct brkfs_sb_info *sbi, u32 *out_ino);
int brkfs_inode_free(struct brkfs_sb_info *sbi, u32 ino);
int brkfs_data_alloc(struct brkfs_sb_info *sbi, u32 *bno);
int brkfs_data_free(struct brkfs_sb_info *sbi, u32 bno);
int brkfs_block_read(struct brkfs_sb_info *sb, u32 bno, void *buf);
int brkfs_block_write(struct brkfs_sb_info *sb, u32 bno, const void *buf);

int brkfs_bitmap_alloc(struct brkfs_sb_info *sbi, u32 start_bno, u32 nbits,
		       u32 *out_bit);
int brkfs_bitmap_free(struct brkfs_sb_info *sbi, u32 start_bno, u32 nbits,
		      u32 bit);

int brkfs_disk_inode_init(struct brkfs_sb_info *sbi, u32 ino, umode_t mode,
			  unsigned int nlink, dev_t rdev);

int brkfs_dir_lookup(struct fs_inode *dir, const char *name,
		     unsigned int name_len, u32 *ino_out, u8 *type_out);
int brkfs_dir_add(struct fs_inode *dir, u32 child_ino, const char *name,
		  unsigned int name_len, umode_t child_mode);
int brkfs_dir_remove(struct fs_inode *dir, const char *name,
		     unsigned int name_len);
int brkfs_dir_is_empty(struct fs_inode *dir);
int brkfs_new_dir_body(struct fs_inode *inode, u32 parent_ino);

int brkfs_file_read_at(struct fs_inode *inode, loff_t *pos, void *buf,
		       size_t size, size_t *read_out);
int brkfs_file_write_at(struct fs_inode *inode, loff_t *pos, const void *buf,
			size_t size, size_t *written_out);
int brkfs_truncate_inode_blocks(struct fs_inode *inode, loff_t new_size);

#define BRKFS_GETBLK_CREATE 0x1

int brkfs_inode_getblk(struct fs_inode *inode, loff_t off, u32 *bno,
		       unsigned flags, struct brkfs_sb_info *sbi);

extern const struct page_cache_ops brkfs_file_pc_ops;

#endif
