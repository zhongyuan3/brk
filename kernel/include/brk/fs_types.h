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
	unsigned int valid;
	umode_t mode;
	kuid_t uid;
	kgid_t gid;
	loff_t size;
	struct timespec atime;
	struct timespec mtime;
	struct timespec ctime;
	struct fs_file *file;
};

struct fs_dir_iterator {
	bool (*actor)(struct fs_dir_iterator *, const char *, int, loff_t,
		      uint64_t, unsigned int);
	loff_t pos;
};

#endif
