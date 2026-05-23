#ifndef BRK_FS_TYPES_H
#define BRK_FS_TYPES_H

#include <brk/types.h>
#include <uapi/time.h>

struct fs_driver;
struct super_block;
struct super_block_ops;
struct inode;
struct inode_ops;
struct file;
struct file_ops;
struct dentry;
struct dentry_ops;
struct mount_instance;
struct path;

struct iattr {
	unsigned int ia_valid;
	umode_t ia_mode;
	kuid_t ia_uid;
	kgid_t ia_gid;
	loff_t ia_size;
	struct timespec ia_atime;
	struct timespec ia_mtime;
	struct timespec ia_ctime;

	/*
	 * Not an attribute, but an auxiliary info for filesystems wanting to
	 * implement an ftruncate() like method.  NOTE: filesystem should
	 * check for (ia_valid & ATTR_FILE), and not for (ia_file != NULL).
	 */
	struct file *ia_file;
};

struct dir_iterator {
	/**
	 * actor() - emit a directory entry
	 * @ctx: directory context
	 * @name: name of the entry
	 * @namelen: length of the name
	 * @offset: offset of the entry
	 * @ino: inode number of the entry
	 * @d_type: type of the entry
	 *
	 * Return: true to continue, false to stop.
	 */
	bool (*actor)(struct dir_iterator *ctx, const char *name, int namelen,
		      loff_t offset, u64 ino, unsigned int d_type);

	loff_t pos;
};

#endif
