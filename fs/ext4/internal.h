#ifndef FS_EXT4_INTERNAL_H
#define FS_EXT4_INTERNAL_H

#include <aosd/fs.h>
#include <ext4.h>
#include <ext4_blockdev.h>
#include <uapi/aosd/stat.h>

struct ext4fs_sb_info {
	struct ext4_sblock *s_sb;
	struct ext4_blockdev s_blkdev;
	struct ext4_lock s_fs_lock;
};

struct ext4fs_inode_info {
	uint64_t i_dir_size;
	bool i_is_dir;
	union {
		struct ext4_file i_file;
		struct ext4_dir i_dir;
	};
};

int ext4fs_read_inode(struct inode *inode);
int ext4fs_open_direntry(struct dentry *dir, struct ext4_dir_en *dir_en,
			 struct dentry *en_dentry);
int ext4fs_dir_find_entry(struct dentry *dir, struct dentry *dentry);
struct ext4fs_sb_info *ext4fs_sbi_create(struct blkdev *bd);
void ext4fs_sbi_destroy(struct ext4fs_sb_info *sbi);

#endif
