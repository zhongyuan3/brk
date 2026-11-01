#ifndef BRK_STAT_H
#define BRK_STAT_H

#include <brk/stat_types.h>
#include <uapi/stat.h>
#include <uapi/statfs.h>

/*
 * Convert kernel-internal status into the user ABI layouts.  These are the
 * only places where struct kstat/kstatfs and struct stat/statfs meet.
 */
void kstat_to_stat(const struct kstat *ks, struct stat *st);
void kstatfs_to_statfs(const struct kstatfs *ks, struct statfs *st);

#endif
