#include <aosd/dcache.h>
#include <aosd/errno.h>
#include <aosd/limits.h>
#include <aosd/process.h>
#include <aosd/slab.h>
#include <aosd/string.h>

char *path_get_full(struct dentry *dp)
{
	size_t name_len = 0;
	struct dentry *next = NULL;
	size_t i = PATH_MAX;
	char *path = kmalloc(PATH_MAX);
	if (!path)
		return NULL;

	if (!dp->d_parent) {
		path[0] = '/';
		path[1] = '\0';
		return path;
	}

	dp = dentry_dup(dp);
	while (dp->d_parent) {
		name_len = strlen(dp->d_name);
		if (i < name_len) {
			kfree(path);
			return NULL;
		}
		i -= name_len;
		memcpy(path + i, dp->d_name, name_len);
		if (i < 1) {
			kfree(path);
			return NULL;
		}
		i -= 1;
		path[i] = '/';
		next = dentry_dup(dp->d_parent);
		dentry_put(dp);
		dp = next;
	}
	dentry_put(dp);
	if (i < 1) {
		kfree(path);
		return NULL;
	}
	memmove(path, path + i, PATH_MAX - i);
	path[PATH_MAX - i] = '\0';
	return path;
}

int path_cat(char *dst, const char *src, size_t src_len)
{
	size_t dst_len;

	if (!src || src_len < 1)
		return 0;

	while (src[0] == '/' && src_len > 0) {
		++src;
		--src_len;
	}

	if (src[0] == '/' && src_len < 1)
		return -EINVAL;

	if (src_len < 1)
		return 0;

	dst_len = strlen(dst);
	if (dst_len + 1 >= PATH_MAX)
		return -1;
	if (dst[dst_len - 1] != '/') {
		if (dst_len + 1 + src_len + 1 > PATH_MAX)
			return -1;
		dst[dst_len] = '/';
		dst_len += 1;
	}
	if (dst_len + src_len + 1 > PATH_MAX)
		return -1;
	memcpy(dst + dst_len, src, src_len);
	dst[dst_len + src_len] = '\0';
	return 0;
}

static const char *path_skip_item(const char *path, char *namebuf,
				  size_t bufsize)
{
	const char *start;
	size_t len;

	while (*path == '/')
		path++;

	if (*path == '\0')
		return NULL;

	start = path;
	while (*path != '/' && *path != '\0')
		path++;

	len = path - start;
	if (len >= bufsize)
		len = bufsize - 1;

	memcpy(namebuf, start, len);
	namebuf[len] = '\0';

	while (*path == '/')
		path++;

	return path;
}

static struct dentry *path_lookup_core(struct dentry *dirdp, const char *path,
				       char *namebuf, size_t bufsize,
				       bool brk_at_parent)
{
	struct dentry *next = NULL;
	dirdp = dentry_dup(dirdp);
	while ((path = path_skip_item(path, namebuf, bufsize)) != NULL) {
		if (brk_at_parent && *path == '\0')
			return dirdp;
		if (strcmp(namebuf, ".") == 0) {
			next = dentry_dup(dirdp);
		} else if (strcmp(namebuf, "..") == 0) {
			if (dirdp->d_parent)
				next = dentry_dup(dirdp->d_parent);
			else
				next = dentry_dup(dirdp);
		} else {
			next = dentry_get(dirdp, namebuf);
		}
		dentry_put(dirdp);
		if (!next)
			return NULL;
		dirdp = next;
	}
	if (brk_at_parent) {
		dentry_put(dirdp);
		return NULL;
	}
	return dirdp;
}

struct dentry *path_lookup_at(struct dentry *dir, const char *path)
{
	char name[NAME_MAX];
	return path_lookup_core(dir, path, name, NAME_MAX, false);
}

struct dentry *path_lookup_parent_at(struct dentry *dir, const char *path,
				     char *name, size_t size)
{
	return path_lookup_core(dir, path, name, size, true);
}

struct dentry *path_lookup(const char *path)
{
	struct dentry *dirdp, *dp;
	char name[NAME_MAX];

	if (path[0] == '/')
		dirdp = dentry_get(NULL, "/");
	else
		dirdp = dentry_dup(current_task()->cwd);

	dp = path_lookup_core(dirdp, path, name, NAME_MAX, false);
	dentry_put(dirdp);
	return dp;
}

struct dentry *path_lookup_parent(const char *path, char *name, size_t size)
{
	struct dentry *dirdp, *dp;

	if (path[0] == '/')
		dirdp = dentry_get(NULL, "/");
	else
		dirdp = dentry_dup(current_task()->cwd);

	dp = path_lookup_core(dirdp, path, name, size, true);
	dentry_put(dirdp);
	return dp;
}

int path_get_last(const char *path, const char **pstart, size_t *plen)
{
	const char *end = NULL;
	const char *last_slash = NULL;
	size_t len = 0;

	if (!path || !pstart || !plen)
		return -1;

	*pstart = path;
	*plen = 0;

	len = strlen(path);
	if (len == 0)
		return 0;

	if (len == 1 && (*path == '/' || *path == '\\')) {
		*pstart = path;
		*plen = 1;
		return 0;
	}

	end = path + len;

	while (end > path && (*(end - 1) == '/' || *(end - 1) == '\\'))
		end--;

	if (end == path) {
		*pstart = path;
		*plen = 1;
		return 0;
	}

	last_slash = end - 1;
	while (last_slash >= path && *last_slash != '/' &&
	       *last_slash != '\\') {
		last_slash--;
	}

	*pstart = last_slash + 1;
	*plen = end - (last_slash + 1);

	return 0;
}
