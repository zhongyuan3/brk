#include <brk/fs.h>
#include <brk/permission.h>
#include <brk/task.h>
#include <uapi/errno.h>
#include <uapi/stat.h>

/*
 * Minimal DAC model: fsuid 0 bypasses permission checks, but root still
 * needs at least one execute bit to execute a regular file.
 */
int inode_permission(const struct fs_inode *inode, int mask)
{
	struct task *task = current_task();
	umode_t mode;
	umode_t perm;

	if (!inode)
		return -ENOENT;
	if (!mask)
		return 0;

	mode = inode->mode;

	if (task->fsuid == 0) {
		if (mask == MAY_EXEC && S_ISREG(mode) && !(mode & 0111))
			return -EACCES;
		return 0;
	}

	if (inode->uid == task->fsuid)
		perm = (mode >> 6) & 7;
	else if (inode->gid == task->fsgid)
		perm = (mode >> 3) & 7;
	else
		perm = mode & 7;

	return (perm & (umode_t)mask) == (umode_t)mask ? 0 : -EACCES;
}

bool inode_owner_or_capable(const struct fs_inode *inode)
{
	struct task *task = current_task();

	if (!inode)
		return false;
	return task->fsuid == 0 || inode->uid == task->fsuid;
}
