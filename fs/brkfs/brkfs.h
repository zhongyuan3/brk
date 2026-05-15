#ifndef BRKFS_H
#define BRKFS_H

#include <brk/dev.h>
#include <brk/fs_types.h>
#include <brk/pagecache.h>
#include <brk/types.h>

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
	u8 __s_padding[BRKFS_SUPER_SIZE - 40];
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
};

struct brkfs_dir_entry {
	u32 inode;
	u16 entry_len;
	u8 name_len;
	u8 file_type;
	char name[BRKFS_NAME_LEN];
};

struct brkfs_sb_info {
	struct blkdev *s_bdev;
	struct brkfs_super_block s_sb;
	u32 s_inodes_per_block;
	u32 s_bits_per_block;
};

struct brkfs_inode_info {
	u32 i_block[BRKFS_BLOCKS];
};

struct brkfs_sb_info *brkfs_sb_info_alloc(struct blkdev *bdev,
					  struct brkfs_super_block *sb);
void brkfs_sb_info_free(struct brkfs_sb_info *sbi);
int brkfs_inode_read(struct brkfs_sb_info *sbi, struct inode *inode);
void brkfs_inode_setup_ops(struct inode *inode);
int brkfs_inode_write(struct brkfs_sb_info *sbi, struct inode *inode);
int brkfs_inode_alloc(struct brkfs_sb_info *sbi, u32 *out_ino);
int brkfs_inode_free(struct brkfs_sb_info *sbi, u32 ino);
int brkfs_data_alloc(struct brkfs_sb_info *sbi, u32 *bno);
int brkfs_data_free(struct brkfs_sb_info *sbi, u32 bno);
int brkfs_block_read(struct brkfs_sb_info *sb, u32 bno, void *buf);
int brkfs_block_write(struct brkfs_sb_info *sb, u32 bno, const void *buf);

int brkfs_disk_inode_init(struct brkfs_sb_info *sbi, u32 ino, umode_t mode,
			  unsigned int nlink, dev_t rdev);

int brkfs_dir_lookup(struct inode *dir, const char *name, unsigned int name_len,
		     u32 *ino_out, u8 *type_out);
int brkfs_dir_add(struct inode *dir, u32 child_ino, const char *name,
		  unsigned int name_len, umode_t child_mode);
int brkfs_dir_remove(struct inode *dir, const char *name,
		     unsigned int name_len);
int brkfs_new_dir_body(struct inode *inode, u32 parent_ino);

int brkfs_file_read_at(struct inode *inode, loff_t *pos, void *buf,
		       usize_t size, usize_t *read_out);
int brkfs_file_write_at(struct inode *inode, loff_t *pos, const void *buf,
			usize_t size, usize_t *written_out);
int brkfs_truncate_inode_blocks(struct inode *inode, loff_t new_size);

#define BRKFS_GETBLK_CREATE 0x1

int brkfs_inode_getblk(struct inode *inode, loff_t off, u32 *bno,
		       unsigned flags, struct brkfs_sb_info *sbi);

extern const struct address_space_operations brkfs_aops;

#endif
