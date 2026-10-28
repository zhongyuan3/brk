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

/* fs_iattr.valid bits */
#define ATTR_MODE (1u << 0)
#define ATTR_UID (1u << 1)
#define ATTR_GID (1u << 2)
#define ATTR_SIZE (1u << 3)
#define ATTR_ATIME (1u << 4)
#define ATTR_MTIME (1u << 5)
#define ATTR_CTIME (1u << 6)

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
	bool (*actor)(struct fs_dir_iterator *, const char *, int, loff_t, u64,
		      unsigned int);
	loff_t pos;
};

#endif
