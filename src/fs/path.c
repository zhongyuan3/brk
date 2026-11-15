#include <brk/assert.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/permission.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <brk/task.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/limits.h>
#include <uapi/stat.h>

static void follow_mount(struct path *path)
{
	if (!dentry_is_mounted(path->dentry))
		return;

	struct mount_state *child_mnt;
	/* May have multiple nested mounts, traverse in loop */
	while ((child_mnt = mount_state_lookup(path))) {
		path_put(path);
		path->mnt = child_mnt;
		path->dentry = dentry_get(child_mnt->root);

		if (!dentry_is_mounted(path->dentry))
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

	result->mnt = mount_state_get(dir->mnt);
	result->dentry = dentry;
	return 0;
}

static int path_init(int dir_fd, const char **pathname, struct path *path)
{
	struct thread *task = current_thread();

	if ((*pathname)[0] == '/') {
		fsinfo_get_root(&task->process->fsinfo, path);
		*pathname += 1;
		return 0;
	}

	if (dir_fd == AT_FDCWD) {
		fsinfo_get_cwd(&task->process->fsinfo, path);
		return 0;
	}

	struct file *f = fdtable_get_file(&task->process->fdtable, dir_fd);
	if (IS_ERR(f))
		return PTR_ERR(f);
	path_get(&f->path);
	*path = f->path;
	file_put(f);
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
		if (cur.dentry != cur.mnt->root) {
			dotdot->dentry = dentry_get(cur.dentry->parent);
			dotdot->mnt = mount_state_get(cur.mnt);
			path_put(&cur);
			return 0;
		}

		if (cur.dentry == cur.mnt->root && cur.mnt->parent &&
		    cur.mnt != cur.mnt->parent) {
			struct mount_state *pmnt =
				mount_state_get(cur.mnt->parent);
			struct dentry *mp = dentry_get(cur.mnt->mount_point);

			path_put(&cur);
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

#define MAXSYMLINKS 40

static int path_walk(struct path *path, const char *name, unsigned int flags,
		     struct qstr *component_name)
{
	int err;
	int depth = 0;
	const char *p = name;
	char target[PATH_MAX];
	char restbuf[PATH_MAX];
	char combined[PATH_MAX];

	while ((p = skip_component(p, component_name))) {
		struct inode *dir_inode = dentry_inode(path->dentry);

		if (dentry_is_negative(path->dentry) || !dir_inode) {
			err = -ENOENT;
			goto lookup_error;
		}

		if (!S_ISDIR(dir_inode->mode)) {
			err = -ENOTDIR;
			goto lookup_error;
		}

		follow_mount(path);

		err = inode_permission(dir_inode, MAY_EXEC);
		if (err)
			goto lookup_error;

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
			err = path_dot(path, &component_path);
		} else if (component_name->len == 2 &&
			   component_name->name[0] == '.' &&
			   component_name->name[1] == '.') {
			err = path_dot_dot(path, &component_path);
		} else {
			err = dir_lookup_entry(path, component_name,
					       &component_path);
		}
		if (err)
			goto lookup_error;

		struct inode *inode = dentry_inode(component_path.dentry);

		if (inode && S_ISLNK(inode->mode)) {
			if (flags & LOOKUP_NO_SYMLINKS) {
				path_put(&component_path);
				err = -ELOOP;
				goto lookup_error;
			}
			if (*p != '\0' || (flags & LOOKUP_FOLLOW)) {
				size_t tlen, rlen, off;
				int len;

				if (depth++ >= MAXSYMLINKS) {
					path_put(&component_path);
					err = -ELOOP;
					goto lookup_error;
				}
				if (!inode->ops || !inode->ops->readlink) {
					path_put(&component_path);
					err = -EINVAL;
					goto lookup_error;
				}
				len = inode->ops->readlink(
					component_path.dentry, target,
					(int)sizeof(target) - 1);
				if (len < 0) {
					path_put(&component_path);
					err = len;
					goto lookup_error;
				}
				if ((size_t)len >= sizeof(target) - 1) {
					path_put(&component_path);
					err = -ENAMETOOLONG;
					goto lookup_error;
				}
				target[len] = '\0';
				if (!target[0]) {
					path_put(&component_path);
					err = -ENOENT;
					goto lookup_error;
				}

				tlen = strlen(target);
				rlen = strlen(p);
				if (tlen + (p[0] ? 1 : 0) + rlen + 1 >
				    sizeof(combined)) {
					path_put(&component_path);
					err = -ENAMETOOLONG;
					goto lookup_error;
				}

				memcpy(restbuf, p, rlen + 1);
				memcpy(combined, target, tlen);
				off = tlen;
				if (p[0])
					combined[off++] = '/';
				memcpy(combined + off, restbuf, rlen + 1);

				path_put(&component_path);

				if (target[0] == '/') {
					const char *q = combined;

					path_put(path);
					memset(path, 0, sizeof(*path));
					err = path_init(AT_FDCWD, &q, path);
					if (err)
						goto lookup_error;
					p = q;
				} else {
					p = combined;
				}
				continue;
			}
		}

		path_put(path);
		*path = component_path;

		/*
		 * LOOKUP_MOUNTPOINT asks for the mountpoint dentry as seen
		 * from its parent mount, not the root of a filesystem mounted
		 * on it.  This is what mount/umount need to identify the
		 * mount_state that lives at the last component.
		 */
		if (!(flags & LOOKUP_MOUNTPOINT) || *p != '\0')
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

static int __path_lookup(int dirfd, const char *name, unsigned int flags,
			 struct path *path, struct qstr *component_name)
{
	int err = path_init(dirfd, &name, path);
	if (err)
		return err;

	return path_walk(path, name, flags, component_name);
}

int path_lookup_at(int dir_fd, const char *name, unsigned int flags,
		   struct path *path)
{
	struct qstr component_name = { 0 };
	return __path_lookup(dir_fd, name, flags, path, &component_name);
}

int path_lookup(const char *name, unsigned int flags, struct path *path)
{
	return path_lookup_at(AT_FDCWD, name, flags, path);
}

void path_get(struct path *path)
{
	if (!path->dentry || !path->mnt)
		return;
	dentry_get(path->dentry);
	mount_state_get(path->mnt);
}

void path_put(struct path *path)
{
	if (!path->dentry || !path->mnt)
		return;
	dentry_put(path->dentry);
	mount_state_put(path->mnt);
}

int path_to_absolute(const struct path *path, char *buf, size_t bufsz)
{
	struct path cur;
	size_t pos;

	if (!path || !path->mnt || !path->dentry || !buf || bufsz == 0)
		return -EINVAL;

	cur.mnt = mount_state_get(path->mnt);
	cur.dentry = dentry_get(path->dentry);

	pos = bufsz;
	buf[--pos] = '\0';

	while (1) {
		const char *name;
		size_t len;
		struct dentry *parent;

		if (cur.dentry == cur.mnt->root && cur.mnt->parent &&
		    cur.mnt != cur.mnt->parent) {
			struct mount_state *pmnt =
				mount_state_get(cur.mnt->parent);
			struct dentry *mp = dentry_get(cur.mnt->mount_point);

			path_put(&cur);
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
			path_put(&cur);
			return -ENAMETOOLONG;
		}

		pos -= len;
		memcpy(buf + pos, name, len);
		buf[--pos] = '/';

		parent = dentry_get(cur.dentry->parent);
		{
			struct mount_state *same_mnt = mount_state_get(cur.mnt);

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

int path_parent_at(int dir_fd, const char *name, struct path *path,
		   struct qstr *last_component)
{
	return __path_lookup(dir_fd, name, LOOKUP_PARENT, path, last_component);
}
