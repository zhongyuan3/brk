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

	/*
	 * Not an attribute, but an auxiliary info for filesystems wanting to
	 * implement an ftruncate() like method.  NOTE: filesystem should
	 * check for (ia_valid & ATTR_FILE), and not for (ia_file != NULL).
	 */
	struct fs_file *ia_file;
};

struct fs_dir_iterator {
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
	bool (*actor)(struct fs_dir_iterator *ctx, const char *name,
		      int namelen, loff_t offset, u64 ino, unsigned int d_type);

	loff_t pos;
};

#endif
