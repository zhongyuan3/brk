#include <brk/fs/dcache.h>
#include <brk/fs/fdtable.h>
#include <brk/fs/fs.h>
#include <brk/fs/fsinfo.h>
#include <brk/fs/mount.h>
#include <brk/fs/path.h>
#include <brk/kernel/printk.h>
#include <brk/kernel/task.h>
#include <brk/lib/assert.h>
#include <brk/lib/error.h>
#include <brk/lib/string.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/limits.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

static void follow_mount(struct fs_path *path)
{
	if (!(path->dentry->flags & DCACHE_MOUNTED))
		return;

	struct fs_mount_state *child_mnt;
	/* May have multiple nested mounts, traverse in loop */
	while ((child_mnt = fs_mount_state_lookup(path))) {
		fs_path_put(path);
		path->mnt = child_mnt;
		path->dentry = fs_dentry_get(child_mnt->root);

		if (!(path->dentry->flags & DCACHE_MOUNTED))
			break;
	}
}

static int dir_lookup_entry(struct fs_path *dir, const struct qstr *name,
			    struct fs_path *result)
{
	struct fs_dentry *parent = dir->dentry;
	struct fs_dentry *dentry;

	dentry = fs_dentry_lookup(parent, name);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	result->mnt = fs_mount_state_get(dir->mnt);
	result->dentry = dentry;
	return 0;
}

static int path_init(int dir_fd, const char **pathname, struct fs_path *path)
{
	struct task_control_block *task = current_task();

	if ((*pathname)[0] == '/') {
		fsinfo_get_root(task->fsinfo, path);
		*pathname += 1;
		return 0;
	}

	if (dir_fd == AT_FDCWD) {
		fsinfo_get_cwd(task->fsinfo, path);
		return 0;
	}

	struct fs_file *f = fdtable_get_file(task->fdtable, dir_fd);
	if (IS_ERR(f))
		return PTR_ERR(f);
	fs_path_get(&f->path);
	*path = f->path;
	fs_file_put(f);
	return 0;
}

static const char *skip_component(const char *pathname,
				  struct qstr *component_name)
{
	const char *p = pathname;

	while (*p == '/')
		p++;

	if (*p == '\0')
		return NULL;

	const char *start = p;
	while (*p != '\0' && *p != '/')
		p++;

	u32 len = p - start;

	*component_name = QSTR_MAKE(start, len);

	while (*p == '/')
		p++;

	return p;
}

int fs_path_dot(struct fs_path *path, struct fs_path *dot)
{
	fs_path_get(path);
	*dot = *path;
	return 0;
}

int fs_path_dot_dot(struct fs_path *path, struct fs_path *dotdot)
{
	struct fs_path cur;

	fs_path_get(path);
	cur = *path;

	while (1) {
		if (cur.dentry != cur.mnt->root) {
			dotdot->dentry = fs_dentry_get(cur.dentry->parent);
			dotdot->mnt = fs_mount_state_get(cur.mnt);
			fs_path_put(&cur);
			return 0;
		}

		if (cur.dentry == cur.mnt->root && cur.mnt->parent &&
		    cur.mnt != cur.mnt->parent) {
			struct fs_mount_state *pmnt =
				fs_mount_state_get(cur.mnt->parent);
			struct fs_dentry *mp =
				fs_dentry_get(cur.mnt->mount_point);

			fs_path_put(&cur);
			cur.mnt = pmnt;
			cur.dentry = mp;
			continue;
		}

		if (!cur.dentry->parent || cur.dentry == cur.dentry->parent)
			break;
	}

	*dotdot = cur;

	return 0;
}

static int __path_lookup(int dirfd, const char *name, unsigned int flags,
			 struct fs_path *path, struct qstr *component_name)
{
	int err = path_init(dirfd, &name, path);
	if (err)
		return err;

	const char *p = name;

	while ((p = skip_component(p, component_name))) {
		if (path->dentry->flags & DCACHE_NEGATIVE) {
			err = -ENOENT;
			goto lookup_error;
		}

		if (!S_ISDIR(path->dentry->inode->mode)) {
			err = -ENOTDIR;
			goto lookup_error;
		}

		follow_mount(path);

		if ((flags & LOOKUP_PARENT) && *p == '\0') {
			/*
			 * Now, path still points to the last item,
			 * which is the parent of %name.
			 * Break early if LOOKUP_PARENT is set.
			 */
			return 0;
		}

		struct fs_path component_path;
		if (component_name->len == 1 &&
		    component_name->name[0] == '.') {
			klog_debug("%s(): Dot\n", __func__);
			err = fs_path_dot(path, &component_path);
		} else if (component_name->len == 2 &&
			   component_name->name[0] == '.' &&
			   component_name->name[1] == '.') {
			klog_debug("%s(): Dot dot\n", __func__);
			err = fs_path_dot_dot(path, &component_path);
		} else {
			err = dir_lookup_entry(path, component_name,
					       &component_path);
		}
		if (err)
			goto lookup_error;

		fs_path_put(path);
		*path = component_path;

		follow_mount(path);
	}

	if (flags & LOOKUP_PARENT) {
		err = -ENOENT;
		goto lookup_error;
	}

	return 0;

lookup_error:
	fs_path_put(path);
	memset(path, 0, sizeof(*path));
	memset(component_name, 0, sizeof(*component_name));
	return err;
}

int fs_path_lookup_at(int dir_fd, const char *name, unsigned int flags,
		      struct fs_path *path)
{
	struct qstr component_name = { 0 };
	return __path_lookup(dir_fd, name, flags, path, &component_name);
}

int fs_path_lookup(const char *name, unsigned int flags, struct fs_path *path)
{
	return fs_path_lookup_at(AT_FDCWD, name, flags, path);
}

void fs_path_get(struct fs_path *path)
{
	if (!path->dentry || !path->mnt)
		return;
	fs_dentry_get(path->dentry);
	fs_mount_state_get(path->mnt);
}

void fs_path_put(struct fs_path *path)
{
	if (!path->dentry || !path->mnt)
		return;
	fs_dentry_put(path->dentry);
	fs_mount_state_put(path->mnt);
}

int fs_path_to_absolute(const struct fs_path *path, char *buf, usize_t bufsz)
{
	struct fs_path cur;
	usize_t pos;

	if (!path || !path->mnt || !path->dentry || !buf || bufsz == 0)
		return -EINVAL;

	cur.mnt = fs_mount_state_get(path->mnt);
	cur.dentry = fs_dentry_get(path->dentry);

	pos = bufsz;
	buf[--pos] = '\0';

	while (1) {
		const char *name;
		usize_t len;
		struct fs_dentry *parent;

		if (cur.dentry == cur.mnt->root && cur.mnt->parent &&
		    cur.mnt != cur.mnt->parent) {
			struct fs_mount_state *pmnt =
				fs_mount_state_get(cur.mnt->parent);
			struct fs_dentry *mp =
				fs_dentry_get(cur.mnt->mount_point);

			fs_path_put(&cur);
			cur.mnt = pmnt;
			cur.dentry = mp;
			continue;
		}

		if (!cur.dentry->parent || cur.dentry == cur.dentry->parent)
			break;

		name = cur.dentry->name.name;
		len = cur.dentry->name.len;
		if (len == 0)
			break;
		if (pos < len + 1) {
			fs_path_put(&cur);
			return -ENAMETOOLONG;
		}

		pos -= len;
		memcpy(buf + pos, name, len);
		buf[--pos] = '/';

		parent = fs_dentry_get(cur.dentry->parent);
		{
			struct fs_mount_state *same_mnt =
				fs_mount_state_get(cur.mnt);

			fs_path_put(&cur);
			cur.mnt = same_mnt;
			cur.dentry = parent;
		}
	}

	fs_path_put(&cur);

	if (pos == bufsz - 1) {
		if (bufsz < 2)
			return -ENAMETOOLONG;
		buf[0] = '/';
		buf[1] = '\0';
		return 0;
	}

	memmove(buf, buf + pos, bufsz - pos);
	return 0;
}

int fs_path_parent_at(int dir_fd, const char *name, struct fs_path *path,
		      struct qstr *last_component)
{
	return __path_lookup(dir_fd, name, LOOKUP_PARENT, path, last_component);
}
