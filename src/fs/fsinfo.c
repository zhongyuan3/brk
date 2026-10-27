#include <brk/fsinfo.h>
#include <brk/kmalloc.h>
#include <brk/path.h>
#include <brk/refcnt.h>
#include <brk/spinlock.h>

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

	spin_lock(&info->lock);
	fs_path_put(&info->cwd);
	fs_path_put(&info->root);
	spin_unlock(&info->lock);

	kfree(info);
}

struct file_system_info *fsinfo_get(struct file_system_info *info)
{
	refcnt_inc(&info->refcnt);
	return info;
}

void fsinfo_copy(struct file_system_info *dst, struct file_system_info *src)
{
	spin_lock(&src->lock);
	spin_lock(&dst->lock);
	fs_path_get(&src->cwd);
	dst->cwd = src->cwd;
	fs_path_get(&src->root);
	dst->root = src->root;
	spin_unlock(&dst->lock);
	spin_unlock(&src->lock);
}

void fsinfo_set_cwd(struct file_system_info *info, struct fs_path *path)
{
	spin_lock(&info->lock);
	info->cwd = *path;
	spin_unlock(&info->lock);
}

void fsinfo_set_root(struct file_system_info *info, struct fs_path *path)
{
	spin_lock(&info->lock);
	info->root = *path;
	spin_unlock(&info->lock);
}

void fsinfo_update_cwd(struct file_system_info *info, struct fs_path *path)
{
	spin_lock(&info->lock);
	fs_path_put(&info->cwd);
	info->cwd = *path;
	spin_unlock(&info->lock);
}

void fsinfo_update_root(struct file_system_info *info, struct fs_path *path)
{
	spin_lock(&info->lock);
	fs_path_put(&info->root);
	info->root = *path;
	spin_unlock(&info->lock);
}

void fsinfo_get_cwd(struct file_system_info *info, struct fs_path *path)
{
	spin_lock(&info->lock);
	fs_path_get(&info->cwd);
	*path = info->cwd;
	spin_unlock(&info->lock);
}

void fsinfo_get_root(struct file_system_info *info, struct fs_path *path)
{
	spin_lock(&info->lock);
	fs_path_get(&info->root);
	*path = info->root;
	spin_unlock(&info->lock);
}

void fsinfo_free_resources(struct file_system_info *info)
{
	spin_lock(&info->lock);
	fs_path_put(&info->cwd);
	fs_path_put(&info->root);
	info->cwd = (struct fs_path){ 0 };
	info->root = (struct fs_path){ 0 };
	spin_unlock(&info->lock);
}
