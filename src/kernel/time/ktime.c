#include <brk/fs.h>
#include <brk/ktime.h>
#include <brk/spinlock.h>
#include <brk/timekeeper.h>
#include <brk/types.h>
#include <uapi/stat.h>

void ktime_get_mono_ts(struct timespec *ts)
{
	timekeeper_get_mono_ts(ts);
}

void ktime_get_real_ts(struct timespec *ts)
{
	timekeeper_get_real_ts(ts);
}

void ktime_set_real_ts(const struct timespec *ts)
{
	timekeeper_set_real_ts(ts);
}

void ktime_get_real_tv(struct timeval *tv)
{
	struct timespec ts;

	ktime_get_real_ts(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = (suseconds_t)(ts.tv_nsec / (long)NS_PER_US);
}

void ktime_set_real_tv(const struct timeval *tv)
{
	struct timespec ts;

	ts.tv_sec = tv->tv_sec;
	ts.tv_nsec = (long)tv->tv_usec * (long)NS_PER_US;
	ktime_set_real_ts(&ts);
}

time_t ktime_get_real_sec(void)
{
	struct timespec ts;

	ktime_get_real_ts(&ts);
	return ts.tv_sec;
}

void ktime_get_boot_ts(struct timespec *ts)
{
	timekeeper_get_mono_ts(ts);
}

u64 ktime_get_boot_ns(void)
{
	return timekeeper_mono_ns();
}

u64 ktime_nanosleep(const struct timespec *dur, struct timespec *rem)
{
	return timekeeper_nanosleep(dur, rem);
}

void inode_times_set_all_now(struct fs_inode *inode)
{
	struct timespec ts;
	irqstate_t flags;

	ktime_get_real_ts(&ts);

	flags = spin_lock_irqsave(&inode->lock);
	inode->atime = ts;
	inode->mtime = ts;
	inode->ctime = ts;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_times_set(struct fs_inode *inode, const struct timespec *atime,
		     const struct timespec *mtime)
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
	struct timespec ts;
	irqstate_t flags;

	ktime_get_real_ts(&ts);

	flags = spin_lock_irqsave(&inode->lock);
	inode->mtime = ts;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_touch_ctime(struct fs_inode *inode)
{
	struct timespec ts;
	irqstate_t flags;

	ktime_get_real_ts(&ts);

	flags = spin_lock_irqsave(&inode->lock);
	inode->ctime = ts;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_touch_mtime_ctime(struct fs_inode *inode)
{
	struct timespec ts;
	irqstate_t flags;

	ktime_get_real_ts(&ts);

	flags = spin_lock_irqsave(&inode->lock);
	inode->mtime = ts;
	inode->ctime = ts;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_times_to_stat(struct fs_inode *inode, struct stat *st)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	st->st_atime = inode->atime.tv_sec;
	st->st_atime_nsec = (unsigned long)inode->atime.tv_nsec;
	st->st_mtime = inode->mtime.tv_sec;
	st->st_mtime_nsec = (unsigned long)inode->mtime.tv_nsec;
	st->st_ctime = inode->ctime.tv_sec;
	st->st_ctime_nsec = (unsigned long)inode->ctime.tv_nsec;

	spin_unlock_irqrestore(&inode->lock, flags);
}
