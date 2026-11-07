#include <brk/fs.h>
#include <brk/ktime.h>
#include <brk/spinlock.h>
#include <brk/timekeeper.h>
#include <brk/types.h>

ktime_t ktime_get_real(void)
{
	return timekeeper_real();
}

int ktime_set_real(ktime_t t)
{
	return timekeeper_set_real(t);
}

time_t ktime_get_real_sec(void)
{
	return (time_t)(ktime_get_real() / (ktime_t)NS_PER_SEC);
}

u64 ktime_get_boot_ns(void)
{
	return timekeeper_mono_ns();
}

int ktime_nanosleep(ktime_t dur, ktime_t *rem)
{
	return timekeeper_nanosleep(dur, rem);
}

void inode_times_set_all_now(struct fs_inode *inode)
{
	ktime_t now = ktime_get_real();
	irqstate_t flags;

	flags = spin_lock_irqsave(&inode->lock);
	inode->atime = now;
	inode->mtime = now;
	inode->ctime = now;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_times_set(struct fs_inode *inode, const ktime_t *atime,
		     const ktime_t *mtime)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	if (atime)
		inode->atime = *atime;
	if (mtime)
		inode->mtime = *mtime;

	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_touch_mtime(struct fs_inode *inode)
{
	ktime_t now = ktime_get_real();
	irqstate_t flags;

	flags = spin_lock_irqsave(&inode->lock);
	inode->mtime = now;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_touch_ctime(struct fs_inode *inode)
{
	ktime_t now = ktime_get_real();
	irqstate_t flags;

	flags = spin_lock_irqsave(&inode->lock);
	inode->ctime = now;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_touch_mtime_ctime(struct fs_inode *inode)
{
	ktime_t now = ktime_get_real();
	irqstate_t flags;

	flags = spin_lock_irqsave(&inode->lock);
	inode->mtime = now;
	inode->ctime = now;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_times_to_kstat(struct fs_inode *inode, struct kstat *ks)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	ks->atime = inode->atime;
	ks->mtime = inode->mtime;
	ks->ctime = inode->ctime;

	spin_unlock_irqrestore(&inode->lock, flags);
}
