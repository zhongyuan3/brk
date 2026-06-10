#include <brk/fs/fsinfo.h>
#include <brk/fs/path.h>
#include <brk/kernel/refcnt.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>

struct file_system_info *fsinfo_alloc(void)
{
	struct file_system_info *info;

	info = kzalloc(sizeof(struct file_system_info));
	if (!info)
		return NULL;

	refcnt_init(&info->refcnt, 1);
	spinlock_init(&info->lock, "fsinfo");

	return info;
}

void fsinfo_put(struct file_system_info *info)
{
	if (refcnt_dec_fetch(&info->refcnt) > 0)
		return;

	spinlock_acquire(&info->lock);
	fs_path_put(&info->cwd);
	fs_path_put(&info->root);
	spinlock_release(&info->lock);

	kfree(info);
}

struct file_system_info *fsinfo_get(struct file_system_info *info)
{
	refcnt_inc(&info->refcnt);
	return info;
}

void fsinfo_copy(struct file_system_info *dst, struct file_system_info *src)
{
	spinlock_acquire(&src->lock);
	spinlock_acquire(&dst->lock);
	fs_path_get(&src->cwd);
	dst->cwd = src->cwd;
	fs_path_get(&src->root);
	dst->root = src->root;
	spinlock_release(&dst->lock);
	spinlock_release(&src->lock);
}

void fsinfo_set_cwd(struct file_system_info *info, struct fs_path *path)
{
	spinlock_acquire(&info->lock);
	info->cwd = *path;
	spinlock_release(&info->lock);
}

void fsinfo_set_root(struct file_system_info *info, struct fs_path *path)
{
	spinlock_acquire(&info->lock);
	info->root = *path;
	spinlock_release(&info->lock);
}

void fsinfo_update_cwd(struct file_system_info *info, struct fs_path *path)
{
	spinlock_acquire(&info->lock);
	fs_path_put(&info->cwd);
	info->cwd = *path;
	spinlock_release(&info->lock);
}

void fsinfo_update_root(struct file_system_info *info, struct fs_path *path)
{
	spinlock_acquire(&info->lock);
	fs_path_put(&info->root);
	info->root = *path;
	spinlock_release(&info->lock);
}

void fsinfo_get_cwd(struct file_system_info *info, struct fs_path *path)
{
	spinlock_acquire(&info->lock);
	fs_path_get(&info->cwd);
	*path = info->cwd;
	spinlock_release(&info->lock);
}

void fsinfo_get_root(struct file_system_info *info, struct fs_path *path)
{
	spinlock_acquire(&info->lock);
	fs_path_get(&info->root);
	*path = info->root;
	spinlock_release(&info->lock);
}

void fsinfo_free_resources(struct file_system_info *info)
{
	spinlock_acquire(&info->lock);
	fs_path_put(&info->cwd);
	fs_path_put(&info->root);
	info->cwd = (struct fs_path){ 0 };
	info->root = (struct fs_path){ 0 };
	spinlock_release(&info->lock);
}
