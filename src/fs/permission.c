#include <brk/fs.h>
#include <brk/permission.h>
#include <brk/task.h>
#include <uapi/errno.h>
#include <uapi/stat.h>

/*
 * Minimal DAC model: CAP_DAC_OVERRIDE bypasses permission checks, but a
 * regular file still needs at least one execute bit to be executable.
 */
static bool task_in_group(const struct task *task, kgid_t gid)
{
	for (int i = 0; i < task->ngroups; i++) {
		if (task->groups[i] == gid)
			return true;
	}
	return false;
}

bool capable(int cap)
{
	if (cap < 0 || cap > CAP_LAST_CAP)
		return false;
	return (current_task()->cap_effective & (1ULL << cap)) != 0;
}

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

	if (capable(CAP_DAC_OVERRIDE)) {
		if (mask == MAY_EXEC && S_ISREG(mode) && !(mode & 0111))
			return -EACCES;
		return 0;
	}

	if (inode->uid == task->fsuid)
		perm = (mode >> 6) & 7;
	else if (inode->gid == task->fsgid || task_in_group(task, inode->gid))
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
	return capable(CAP_FOWNER) || inode->uid == task->fsuid;
}
