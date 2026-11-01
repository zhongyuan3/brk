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
	__u64 ino;
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
	__s64 type;
	__s64 bsize;
	__s64 blocks;
	__s64 bfree;
	__s64 bavail;
	__s64 files;
	__s64 ffree;
	__s64 fsid;
	__s64 namelen;
	__s64 frsize;
	__s64 flags;
};

#endif
