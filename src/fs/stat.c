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

void kstatfs_to_statfs(const struct kstatfs *ks, struct brk_statfs *st)
{
	st->type = ks->type;
	st->bsize = ks->bsize;
	st->frsize = ks->frsize;
	st->blocks = ks->blocks;
	st->bfree = ks->bfree;
	st->bavail = ks->bavail;
	st->files = ks->files;
	st->ffree = ks->ffree;
	st->fsid = ks->fsid;
	st->namelen = ks->namelen;
	st->flags = ks->flags;
}
