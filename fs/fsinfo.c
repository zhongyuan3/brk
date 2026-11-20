#include <brk/fsinfo.h>
#include <brk/kmalloc.h>
#include <brk/path.h>
#include <brk/spinlock.h>
#include <brk/string.h>

void fsinfo_init(struct file_system_info *info)
{
	memset(info, 0, sizeof(*info));
	spinlock_init(&info->lock, "fsinfo");
}

void fsinfo_deinit(struct file_system_info *info)
{
	struct path cwd, root;

	/*
	 * path_put() can evict the last dentry/inode and therefore sleep
	 * (page-cache / block I/O, or a filesystem sleep lock).  Take the
	 * paths out of the object under the spinlock, then drop the
	 * references with interrupts and preemption enabled.
	 */
	spin_lock(&info->lock);
	cwd = info->cwd;
	root = info->root;
	info->cwd = (struct path){ 0 };
	info->root = (struct path){ 0 };
	spin_unlock(&info->lock);

	path_put(&cwd);
	path_put(&root);
}

void fsinfo_copy(struct file_system_info *dst, struct file_system_info *src)
{
	spin_lock(&src->lock);
	spin_lock(&dst->lock);
	path_get(&src->cwd);
	dst->cwd = src->cwd;
	path_get(&src->root);
	dst->root = src->root;
	spin_unlock(&dst->lock);
	spin_unlock(&src->lock);
}

void fsinfo_set_cwd(struct file_system_info *info, struct path *path)
{
	spin_lock(&info->lock);
	info->cwd = *path;
	spin_unlock(&info->lock);
}

void fsinfo_set_root(struct file_system_info *info, struct path *path)
{
	spin_lock(&info->lock);
	info->root = *path;
	spin_unlock(&info->lock);
}

void fsinfo_update_cwd(struct file_system_info *info, struct path *path)
{
	struct path old;

	spin_lock(&info->lock);
	old = info->cwd;
	info->cwd = *path;
	spin_unlock(&info->lock);

	path_put(&old);
}

void fsinfo_update_root(struct file_system_info *info, struct path *path)
{
	struct path old;

	spin_lock(&info->lock);
	old = info->root;
	info->root = *path;
	spin_unlock(&info->lock);

	path_put(&old);
}

void fsinfo_get_cwd(struct file_system_info *info, struct path *path)
{
	spin_lock(&info->lock);
	path_get(&info->cwd);
	*path = info->cwd;
	spin_unlock(&info->lock);
}

void fsinfo_get_root(struct file_system_info *info, struct path *path)
{
	spin_lock(&info->lock);
	path_get(&info->root);
	*path = info->root;
	spin_unlock(&info->lock);
}
