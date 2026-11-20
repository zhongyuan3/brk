#ifndef BRK_FS_TYPES_H
#define BRK_FS_TYPES_H

#include <brk/types.h>
#include <stdbool.h>

struct file_system_type;
struct super_block;
struct super_block_ops;
struct inode;
struct inode_ops;
struct file;
struct file_ops;
struct dentry;
struct dentry_ops;
struct mount_state;
struct path;

/* iattr.valid bits */
#define ATTR_MODE (1u << 0)
#define ATTR_UID (1u << 1)
#define ATTR_GID (1u << 2)
#define ATTR_SIZE (1u << 3)
#define ATTR_ATIME (1u << 4)
#define ATTR_MTIME (1u << 5)
#define ATTR_CTIME (1u << 6)

struct iattr {
	unsigned int valid;
	umode_t mode;
	kuid_t uid;
	kgid_t gid;
	loff_t size;
	ktime_t atime;
	ktime_t mtime;
	ktime_t ctime;
	struct file *file;
};

struct dir_iterator {
	bool (*actor)(struct dir_iterator *, const char *, int, loff_t, u64,
		      unsigned int);
	loff_t pos;
};

#endif
