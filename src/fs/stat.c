#include <brk/stat.h>
#include <brk/string.h>

void kstat_to_statx(const struct kstat *ks, struct statx *stx)
{
	memset(stx, 0, sizeof(*stx));

	stx->stx_dev = (__u64)ks->dev;
	stx->stx_ino = ks->ino;
	stx->stx_rdev = (__u64)ks->rdev;
	stx->stx_size = ks->size;
	stx->stx_blocks = ks->blocks;
	stx->stx_atime_ns = ks->atime;
	stx->stx_mtime_ns = ks->mtime;
	stx->stx_ctime_ns = ks->ctime;
	stx->stx_mode = ks->mode;
	stx->stx_nlink = ks->nlink;
	stx->stx_uid = ks->uid;
	stx->stx_gid = ks->gid;
	stx->stx_blksize = (__s32)ks->blksize;
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
