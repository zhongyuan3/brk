#include <brk/ktime.h>
#include <brk/stat.h>
#include <brk/string.h>

void kstat_to_stat(const struct kstat *ks, struct stat *st)
{
	struct timespec ts;

	memset(st, 0, sizeof(*st));

	st->st_dev = (__u64)ks->dev;
	st->st_ino = ks->ino;
	st->st_mode = ks->mode;
	st->st_nlink = ks->nlink;
	st->st_uid = ks->uid;
	st->st_gid = ks->gid;
	st->st_rdev = (__u64)ks->rdev;
	st->st_size = ks->size;
	st->st_blksize = (__s32)ks->blksize;
	st->st_blocks = ks->blocks;

	ktime_to_timespec(ks->atime, &ts);
	st->st_atime = ts.tv_sec;
	st->st_atime_nsec = (__u64)ts.tv_nsec;

	ktime_to_timespec(ks->mtime, &ts);
	st->st_mtime = ts.tv_sec;
	st->st_mtime_nsec = (__u64)ts.tv_nsec;

	ktime_to_timespec(ks->ctime, &ts);
	st->st_ctime = ts.tv_sec;
	st->st_ctime_nsec = (__u64)ts.tv_nsec;
}

void kstatfs_to_statvfs(const struct kstatfs *ks, struct statvfs *st)
{
	memset(st, 0, sizeof(*st));

	st->f_bsize = ks->bsize;
	st->f_frsize = ks->frsize;
	st->f_blocks = ks->blocks;
	st->f_bfree = ks->bfree;
	st->f_bavail = ks->bavail;
	st->f_files = ks->files;
	st->f_ffree = ks->ffree;
	st->f_favail = ks->ffree;
	st->f_fsid = ks->fsid;
	st->f_namemax = ks->namelen;

	/* The internal mount flags use the MS_* numbering, whose low bits
	 * coincide with ST_RDONLY and ST_NOSUID. */
	if (ks->flags & 0x1)
		st->f_flag |= ST_RDONLY;
	if (ks->flags & 0x2)
		st->f_flag |= ST_NOSUID;
}
