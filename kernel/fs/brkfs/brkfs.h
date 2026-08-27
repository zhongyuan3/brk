#ifndef BRKFS_H
#define BRKFS_H

#include <brk/blkdev.h>
#include <brk/fs_types.h>
#include <brk/pagecache.h>
#include <brk/types.h>
#include <uapi/brk/types.h>

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

#define BRKFS_NAME_LEN 255

struct brkfs_super_block {
	uint32_t s_blocksize; /* Block size */
	uint32_t s_inode_bitmap; /* Inode bitmap start block number */
	uint32_t s_inodes_count; /* Total number of inodes */
	uint32_t s_data_block_bitmap; /* Data block bitmap start block number */
	uint32_t s_data_blocks_count; /* Total number of data blocks */
	uint32_t s_inode_table; /* Inode table start block number */
	uint32_t s_first_data_block; /* First data block number */
	uint32_t s_magic; /* File system magic number */
	uint32_t s_inode_size; /* Size of an inode */
	uint32_t s_blocks_count; /* Total number of blocks */
	uint8_t __s_padding[BRKFS_SUPER_SIZE - 40];
};

struct brkfs_inode {
	uint32_t i_ino;
	uint32_t i_mode;
	uint32_t i_rdev;
	uint32_t i_flags;
	uint32_t i_nlink;
	uint32_t i_size;
	uint32_t i_atime;
	uint32_t i_atime_nsec;
	uint32_t i_ctime;
	uint32_t i_ctime_nsec;
	uint32_t i_mtime;
	uint32_t i_mtime_nsec;
	uint32_t i_uid;
	uint32_t i_gid;
	uint32_t i_block[BRKFS_BLOCKS];
};

struct brkfs_dir_entry {
	uint32_t inode;
	uint16_t entry_len;
	uint8_t name_len;
	uint8_t file_type;
	char name[BRKFS_NAME_LEN];
};

struct brkfs_sb_info {
	struct block_dev *s_bdev;
	struct brkfs_super_block s_sb;
	uint32_t s_inodes_per_block;
	uint32_t s_bits_per_block;
};

struct brkfs_inode_info {
	uint32_t i_block[BRKFS_BLOCKS];
};

struct brkfs_block {
	struct cached_page *cp;
	void *data;
};

int brkfs_get_block(struct brkfs_sb_info *sbi, uint32_t bno,
		    struct brkfs_block *bb);
void brkfs_put_block(struct brkfs_block *bb);

struct brkfs_sb_info *brkfs_sb_info_alloc(struct block_dev *bdev,
					  struct brkfs_super_block *sb);
void brkfs_sb_info_free(struct brkfs_sb_info *sbi);
int brkfs_inode_read(struct brkfs_sb_info *sbi, struct fs_inode *inode);
void brkfs_inode_setup_ops(struct fs_inode *inode);
int brkfs_inode_write(struct brkfs_sb_info *sbi, struct fs_inode *inode);
int brkfs_inode_alloc(struct brkfs_sb_info *sbi, uint32_t *out_ino);
int brkfs_inode_free(struct brkfs_sb_info *sbi, uint32_t ino);
int brkfs_data_alloc(struct brkfs_sb_info *sbi, uint32_t *bno);
int brkfs_data_free(struct brkfs_sb_info *sbi, uint32_t bno);
int brkfs_block_read(struct brkfs_sb_info *sb, uint32_t bno, void *buf);
int brkfs_block_write(struct brkfs_sb_info *sb, uint32_t bno, const void *buf);

int brkfs_bitmap_alloc(struct brkfs_sb_info *sbi, uint32_t start_bno,
		       uint32_t nbits, uint32_t *out_bit);
int brkfs_bitmap_free(struct brkfs_sb_info *sbi, uint32_t start_bno,
		      uint32_t nbits, uint32_t bit);

int brkfs_disk_inode_init(struct brkfs_sb_info *sbi, uint32_t ino, umode_t mode,
			  unsigned int nlink, dev_t rdev);

int brkfs_dir_lookup(struct fs_inode *dir, const char *name,
		     unsigned int name_len, uint32_t *ino_out,
		     uint8_t *type_out);
int brkfs_dir_add(struct fs_inode *dir, uint32_t child_ino, const char *name,
		  unsigned int name_len, umode_t child_mode);
int brkfs_dir_remove(struct fs_inode *dir, const char *name,
		     unsigned int name_len);
int brkfs_new_dir_body(struct fs_inode *inode, uint32_t parent_ino);

int brkfs_file_read_at(struct fs_inode *inode, loff_t *pos, void *buf,
		       size_t size, size_t *read_out);
int brkfs_file_write_at(struct fs_inode *inode, loff_t *pos, const void *buf,
			size_t size, size_t *written_out);
int brkfs_truncate_inode_blocks(struct fs_inode *inode, loff_t new_size);

#define BRKFS_GETBLK_CREATE 0x1

int brkfs_inode_getblk(struct fs_inode *inode, loff_t off, uint32_t *bno,
		       unsigned flags, struct brkfs_sb_info *sbi);

extern const struct page_cache_ops brkfs_file_pc_ops;

#endif
