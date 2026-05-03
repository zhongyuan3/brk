#ifndef BRK_FS_TYPES_H
#define BRK_FS_TYPES_H

#include <brk/time.h>
#include <brk/types.h>

struct super_block;
struct super_operations;
struct inode;
struct inode_operations;
struct file;
struct file_operations;
struct file_system_type;
struct dentry;
struct mount;
struct path;

struct iattr {
	unsigned int ia_valid;
	umode_t ia_mode;
	uid_t ia_uid;
	gid_t ia_gid;
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

struct dir_context {
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
	bool (*actor)(struct dir_context *ctx, const char *name, int namelen,
		      loff_t offset, uint64_t ino, unsigned int d_type);

	loff_t pos;
};

#endif
