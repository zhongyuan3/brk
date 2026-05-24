#ifndef BRK_FS_TYPES_H
#define BRK_FS_TYPES_H

#include <brk/types.h>
#include <uapi/time.h>

struct fs_driver;
struct fs_super_block;
struct fs_super_block_ops;
struct fs_inode;
struct fs_inode_ops;
struct fs_file;
struct fs_file_ops;
struct fs_dentry;
struct fs_dentry_ops;
struct fs_mount_state;
struct fs_path;

struct fs_iattr {
	unsigned int ia_valid;
	umode_t ia_mode;
	kuid_t ia_uid;
	kgid_t ia_gid;
	loff_t ia_size;
	struct timespec ia_atime;
	struct timespec ia_mtime;
	struct timespec ia_ctime;
	struct fs_file *ia_file;
};

struct fs_dir_iterator {
	bool (*actor)(struct fs_dir_iterator *, const char *, int, loff_t, u64,
		      unsigned int);
	loff_t pos;
};

#endif
