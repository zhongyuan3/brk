#ifndef BRK_STAT_H
#define BRK_STAT_H

#include <brk/stat_types.h>
#include <uapi/stat.h>
#include <uapi/statvfs.h>

/*
 * Convert kernel-internal status into the user ABI layouts.  These are the
 * only places where struct kstat/kstatfs and struct stat/statvfs meet.
 */
void kstat_to_statx(const struct kstat *ks, struct statx *stx);
void kstatfs_to_statvfs(const struct kstatfs *ks, struct statvfs *st);

#endif
