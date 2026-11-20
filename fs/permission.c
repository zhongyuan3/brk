#include <brk/fs.h>
#include <brk/permission.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <uapi/errno.h>
#include <uapi/stat.h>

bool capable(int cap)
{
	return process_has_cap(current_process(), cap);
}

int inode_permission(const struct inode *inode, int mask)
{
	struct process *process = current_process();
	umode_t mode;
	umode_t perm;
	bool cap_override;
	kuid_t fsuid;
	kgid_t fsgid;
	bool in_group = false;
	irqstate_t flags;

	if (!inode)
		return -ENOENT;
	if (!mask)
		return 0;

	mode = inode->mode;

	flags = spin_lock_irqsave(&process->cred_lock);
	cap_override =
		(process->cred.cap_effective & (1ULL << CAP_DAC_OVERRIDE)) != 0;
	fsuid = process->cred.fsuid;
	fsgid = process->cred.fsgid;
	for (int i = 0; i < process->cred.ngroups; i++) {
		if (process->cred.groups[i] == inode->gid) {
			in_group = true;
			break;
		}
	}
	spin_unlock_irqrestore(&process->cred_lock, flags);

	if (cap_override) {
		if (mask == MAY_EXEC && S_ISREG(mode) && !(mode & 0111))
			return -EACCES;
		return 0;
	}

	if (inode->uid == fsuid)
		perm = (mode >> 6) & 7;
	else if (inode->gid == fsgid || in_group)
		perm = (mode >> 3) & 7;
	else
		perm = mode & 7;

	return (perm & (umode_t)mask) == (umode_t)mask ? 0 : -EACCES;
}

bool inode_owner_or_capable(const struct inode *inode)
{
	struct process *process = current_process();
	bool cap_owner;
	kuid_t fsuid;
	irqstate_t flags;

	if (!inode)
		return false;

	flags = spin_lock_irqsave(&process->cred_lock);
	cap_owner = (process->cred.cap_effective & (1ULL << CAP_FOWNER)) != 0;
	fsuid = process->cred.fsuid;
	spin_unlock_irqrestore(&process->cred_lock, flags);

	return cap_owner || inode->uid == fsuid;
}
