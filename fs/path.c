#include <brk/assert.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/string.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/limits.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

static void follow_mount(struct path *path)
{
	if (!(path->dentry->d_flags & DCACHE_MOUNTED))
		return;

	struct mount_instance *child_mnt;
	/* May have multiple nested mounts, traverse in loop */
	while ((child_mnt = lookup_mount(path))) {
		path_put(path);
		path->mnt = child_mnt;
		path->dentry = dentry_get(child_mnt->mnt_root);

		if (!(path->dentry->d_flags & DCACHE_MOUNTED))
			break;
	}
}

static int dir_lookup_entry(struct path *dir, const struct qstr *name,
			    struct path *result)
{
	struct dentry *parent = dir->dentry;
	struct dentry *dentry;

	dentry = dentry_lookup(parent, name);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	result->mnt = mount_get(dir->mnt);
	result->dentry = dentry;
	return 0;
}

static int path_init(int dir_fd, const char **pathname, struct path *path)
{
	struct process *proc = current_process();

	if ((*pathname)[0] == '/') {
		path_get(&proc->root);
		*path = proc->root;
		*pathname += 1;
		return 0;
	}

	if (dir_fd == AT_FDCWD) {
		path_get(&proc->cwd);
		*path = proc->cwd;
		return 0;
	}

	if (0 <= dir_fd && dir_fd < OPEN_MAX && proc->ofiles[dir_fd]) {
		path_get(&proc->ofiles[dir_fd]->f_path);
		*path = proc->ofiles[dir_fd]->f_path;
		return 0;
	}

	return -EBADF;
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

int path_dot(struct path *path, struct path *dot)
{
	path_get(path);
	*dot = *path;
	return 0;
}

int path_dot_dot(struct path *path, struct path *dotdot)
{
	struct path cur;

	path_get(path);
	cur = *path;

	while (1) {
		if (cur.dentry != cur.mnt->mnt_root) {
			dotdot->dentry = dentry_get(cur.dentry->d_parent);
			dotdot->mnt = mount_get(cur.mnt);
			path_put(&cur);
			return 0;
		}

		if (cur.dentry == cur.mnt->mnt_root && cur.mnt->mnt_parent &&
		    cur.mnt != cur.mnt->mnt_parent) {
			struct mount_instance *pmnt =
				mount_get(cur.mnt->mnt_parent);
			struct dentry *mp = dentry_get(cur.mnt->mnt_mountpoint);

			path_put(&cur);
			cur.mnt = pmnt;
			cur.dentry = mp;
			continue;
		}

		if (!cur.dentry->d_parent || cur.dentry == cur.dentry->d_parent)
			break;
	}

	*dotdot = cur;

	return 0;
}

static int __path_lookup(int dirfd, const char *name, unsigned int flags,
			 struct path *path, struct qstr *component_name)
{
	int err = path_init(dirfd, &name, path);
	if (err)
		return err;

	const char *p = name;

	while ((p = skip_component(p, component_name))) {
		if (path->dentry->d_flags & DCACHE_NEGATIVE) {
			err = -ENOENT;
			goto lookup_error;
		}

		if (!S_ISDIR(path->dentry->d_inode->i_mode)) {
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

		struct path component_path;
		if (component_name->len == 1 &&
		    component_name->name[0] == '.') {
			klog_debug("%s(): Dot\n", __func__);
			err = path_dot(path, &component_path);
		} else if (component_name->len == 2 &&
			   component_name->name[0] == '.' &&
			   component_name->name[1] == '.') {
			klog_debug("%s(): Dot dot\n", __func__);
			err = path_dot_dot(path, &component_path);
		} else {
			err = dir_lookup_entry(path, component_name,
					       &component_path);
		}
		if (err)
			goto lookup_error;

		path_put(path);
		*path = component_path;

		follow_mount(path);
	}

	if (flags & LOOKUP_PARENT) {
		err = -ENOENT;
		goto lookup_error;
	}

	return 0;

lookup_error:
	path_put(path);
	memset(path, 0, sizeof(*path));
	memset(component_name, 0, sizeof(*component_name));
	return err;
}

int path_lookupat(int dir_fd, const char *name, unsigned int flags,
		  struct path *path)
{
	struct qstr component_name = { 0 };
	return __path_lookup(dir_fd, name, flags, path, &component_name);
}

int path_lookup(const char *name, unsigned int flags, struct path *path)
{
	return path_lookupat(AT_FDCWD, name, flags, path);
}

void path_get(struct path *path)
{
	dentry_get(path->dentry);
	mount_get(path->mnt);
}

void path_put(struct path *path)
{
	dentry_put(path->dentry);
	mount_put(path->mnt);
}

int path_to_absolute(const struct path *path, char *buf, usize_t bufsz)
{
	struct path cur;
	usize_t pos;

	if (!path || !path->mnt || !path->dentry || !buf || bufsz == 0)
		return -EINVAL;

	cur.mnt = mount_get(path->mnt);
	cur.dentry = dentry_get(path->dentry);

	pos = bufsz;
	buf[--pos] = '\0';

	while (1) {
		const char *name;
		usize_t len;
		struct dentry *parent;

		if (cur.dentry == cur.mnt->mnt_root && cur.mnt->mnt_parent &&
		    cur.mnt != cur.mnt->mnt_parent) {
			struct mount_instance *pmnt =
				mount_get(cur.mnt->mnt_parent);
			struct dentry *mp = dentry_get(cur.mnt->mnt_mountpoint);

			path_put(&cur);
			cur.mnt = pmnt;
			cur.dentry = mp;
			continue;
		}

		if (!cur.dentry->d_parent || cur.dentry == cur.dentry->d_parent)
			break;

		name = cur.dentry->d_name.name;
		len = cur.dentry->d_name.len;
		if (len == 0)
			break;
		if (pos < len + 1) {
			path_put(&cur);
			return -ENAMETOOLONG;
		}

		pos -= len;
		memcpy(buf + pos, name, len);
		buf[--pos] = '/';

		parent = dentry_get(cur.dentry->d_parent);
		{
			struct mount_instance *same_mnt = mount_get(cur.mnt);

			path_put(&cur);
			cur.mnt = same_mnt;
			cur.dentry = parent;
		}
	}

	path_put(&cur);

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

int path_parentat(int dir_fd, const char *name, struct path *path,
		  struct qstr *last_component)
{
	return __path_lookup(dir_fd, name, LOOKUP_PARENT, path, last_component);
}
