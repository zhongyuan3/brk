#ifndef BRK_STAT_TYPES_H
#define BRK_STAT_TYPES_H

#include <brk/types.h>

/*
 * Kernel-internal file status.  This is deliberately distinct from the
 * user-visible struct stat: the VFS and filesystems work with kstat, and only
 * the syscall layer converts it to the ABI layout.
 *
 * Times are nanoseconds since the epoch (ktime_t).
 */
struct kstat {
	dev_t dev;
	__brk_u64 ino;
	umode_t mode;
	unsigned int nlink;
	kuid_t uid;
	kgid_t gid;
	dev_t rdev;
	loff_t size;
	long blksize;
	loff_t blocks;
	ktime_t atime;
	ktime_t mtime;
	ktime_t ctime;
};

/* Kernel-internal filesystem statistics, converted at the syscall edge. */
struct kstatfs {
	__brk_s64 type;
	__brk_s64 bsize;
	__brk_s64 blocks;
	__brk_s64 bfree;
	__brk_s64 bavail;
	__brk_s64 files;
	__brk_s64 ffree;
	__brk_s64 fsid;
	__brk_s64 namelen;
	__brk_s64 frsize;
	__brk_s64 flags;
};

#endif
