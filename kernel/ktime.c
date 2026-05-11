#include <brk/fs.h>
#include <brk/ktime.h>
#include <brk/stat.h>
#include <brk/timer.h>

void ktime_get_real_ts(struct timespec *ts)
{
	struct timeval tv;

	walltime_get(&tv);
	ts->tv_sec = tv.tv_sec;
	ts->tv_nsec = (long)tv.tv_usec * 1000L;
}

time_t ktime_get_real_sec(void)
{
	struct timespec ts;

	ktime_get_real_ts(&ts);
	return ts.tv_sec;
}

void inode_times_set_all_now(struct inode *inode)
{
	ktime_get_real_ts(&inode->i_atime);
	inode->i_mtime = inode->i_atime;
	inode->i_ctime = inode->i_atime;
}

void inode_touch_mtime(struct inode *inode)
{
	ktime_get_real_ts(&inode->i_mtime);
}

void inode_touch_ctime(struct inode *inode)
{
	ktime_get_real_ts(&inode->i_ctime);
}

void inode_touch_mtime_ctime(struct inode *inode)
{
	inode_touch_mtime(inode);
	inode_touch_ctime(inode);
}

void inode_times_to_stat(const struct inode *inode, struct stat *st)
{
	st->st_atime = inode->i_atime.tv_sec;
	st->st_atime_nsec = (unsigned long)inode->i_atime.tv_nsec;
	st->st_mtime = inode->i_mtime.tv_sec;
	st->st_mtime_nsec = (unsigned long)inode->i_mtime.tv_nsec;
	st->st_ctime = inode->i_ctime.tv_sec;
	st->st_ctime_nsec = (unsigned long)inode->i_ctime.tv_nsec;
}
