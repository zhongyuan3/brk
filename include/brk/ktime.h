#ifndef BRK_KTIME_H
#define BRK_KTIME_H

#include <brk/stat.h>
#include <brk/time.h>
#include <brk/timeconst.h>
#include <brk/types.h>

struct inode;

void ktime_get_mono_ts(struct timespec *ts);
void ktime_get_real_ts(struct timespec *ts);
void ktime_set_real_ts(const struct timespec *ts);
void ktime_get_real_tv(struct timeval *tv);
void ktime_set_real_tv(const struct timeval *tv);
time_t ktime_get_real_sec(void);

void ktime_get_boot_ts(struct timespec *ts);

u64 ktime_nanosleep(const struct timespec *dur, struct timespec *rem);

void inode_times_set_all_now(struct inode *inode);
void inode_touch_mtime(struct inode *inode);
void inode_touch_ctime(struct inode *inode);
void inode_touch_mtime_ctime(struct inode *inode);

void inode_times_to_stat(const struct inode *inode, struct stat *st);

#endif
