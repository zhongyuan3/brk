#ifndef _BRK_STAT_H
#define _BRK_STAT_H

#include <brk/stat_types.h>
#include <uapi/stat.h>
#include <uapi/statvfs.h>

/*
 * Convert kernel-internal status into the user ABI layouts.  These are the
 * only places where struct kstat/kstatfs and struct stat/statvfs meet.
 */
void kstat_to_statx(const struct kstat *ks, struct __brk_stat *stx);
void kstatfs_to_statfs(const struct kstatfs *ks, struct __brk_statfs *st);

#endif
