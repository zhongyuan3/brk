#include <brk/fs.h>
#include <brk/ktime.h>
#include <brk/timekeeper.h>
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

uint64_t ktime_nanosleep(const struct timespec *dur, struct timespec *rem)
{
	return timekeeper_nanosleep(dur, rem);
}

void inode_times_set_all_now(struct fs_inode *inode)
{
	ktime_get_real_ts(&inode->atime);
	inode->mtime = inode->atime;
	inode->ctime = inode->atime;
}

void inode_touch_mtime(struct fs_inode *inode)
{
	ktime_get_real_ts(&inode->mtime);
}

void inode_touch_ctime(struct fs_inode *inode)
{
	ktime_get_real_ts(&inode->ctime);
}

void inode_touch_mtime_ctime(struct fs_inode *inode)
{
	inode_touch_mtime(inode);
	inode_touch_ctime(inode);
}

void inode_times_to_stat(const struct fs_inode *inode, struct stat *st)
{
	st->st_atime = inode->atime.tv_sec;
	st->st_atime_nsec = (unsigned long)inode->atime.tv_nsec;
	st->st_mtime = inode->mtime.tv_sec;
	st->st_mtime_nsec = (unsigned long)inode->mtime.tv_nsec;
	st->st_ctime = inode->ctime.tv_sec;
	st->st_ctime_nsec = (unsigned long)inode->ctime.tv_nsec;
}
