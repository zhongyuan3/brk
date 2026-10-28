#ifndef BRK_DEVTMPFS_H
#define BRK_DEVTMPFS_H

#include <brk/device_model.h>
#include <brk/fs.h>
#include <brk/types.h>

extern struct fs_driver devtmpfs_fs_type;

/*
 * Create /dev nodes for every registered device that has a dev_t.  Used both
 * for the bootstrap filesystem (before the real root is mounted) and after
 * /dev is mounted.  Idempotent: existing nodes are left alone.
 */
int devtmpfs_populate(void);

/*
 * Enable runtime node management: start the kernel thread that creates and
 * removes nodes when devices are registered/unregistered.  Must run in
 * sleepable task context after /dev has been mounted.
 */
int devtmpfs_start(void);

/* Called by the device core; safe in atomic context (queues work only). */
void devtmpfs_notify(struct device *dev, bool remove);

#endif
