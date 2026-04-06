#ifndef FS_EXT4_INTERNAL_H
#define FS_EXT4_INTERNAL_H

#include <aosd/fs.h>
#include <ext4.h>

struct ext4fs_super_block {
	struct ext4_sblock *s_sb;
	struct ext4_blockdev s_blkdev;
};

struct ext4fs_inode {
	uint64_t i_dir_size;
	bool i_is_dir;
	union {
		struct ext4_file i_file;
		struct ext4_dir i_dir;
	};
};

int ext4fs_read_inode_metadata(struct inode *ip);
int ext4fs_dir_find_entry(struct dentry *dir_dp, struct dentry *dp);
int ext4fs_dir_open_entry(struct dentry *dir_dp, struct ext4_dir_en *en,
			  struct dentry *dp);
struct ext4fs_super_block *ext4fs_alloc_sb(struct blkdev *bdev);
void ext4fs_free_sb(struct ext4fs_super_block *sb);

#endif
