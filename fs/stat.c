#include <brk/stat.h>
#include <brk/string.h>

void kstat_to_statx(const struct kstat *ks, struct __brk_stat *stx)
{
	memset(stx, 0, sizeof(*stx));

	stx->st_dev = (__brk_u64)ks->dev;
	stx->st_ino = ks->ino;
	stx->st_rdev = (__brk_u64)ks->rdev;
	stx->st_size = ks->size;
	stx->st_blocks = ks->blocks;
	stx->st_atime_ns = ks->atime;
	stx->st_mtime_ns = ks->mtime;
	stx->st_ctime_ns = ks->ctime;
	stx->st_mode = ks->mode;
	stx->st_nlink = ks->nlink;
	stx->st_uid = ks->uid;
	stx->st_gid = ks->gid;
	stx->st_blksize = (__brk_s32)ks->blksize;
}

void kstatfs_to_statfs(const struct kstatfs *ks, struct __brk_statfs *st)
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
