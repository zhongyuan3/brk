#ifndef BRK_KTIME_H
#define BRK_KTIME_H

#include <brk/stat.h>
#include <brk/time.h>

#define NS_PER_SEC 1000000000
#define NS_PER_MS 1000000
#define NS_PER_US 1000
#define US_PER_SEC 1000000
#define US_PER_MS 1000
#define MS_PER_SEC 1000
#define SEC_PER_MIN 60
#define MIN_PER_HOUR 60
#define HOUR_PER_DAY 24

struct inode;

void ktime_get_real_ts(struct timespec *ts);
time_t ktime_get_real_sec(void);

void inode_times_set_all_now(struct inode *inode);
void inode_touch_mtime(struct inode *inode);
void inode_touch_ctime(struct inode *inode);
void inode_touch_mtime_ctime(struct inode *inode);

void inode_times_to_stat(const struct inode *inode, struct stat *st);

#endif
