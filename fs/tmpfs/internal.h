#ifndef FS_TMPFS_INTERNAL_H
#define FS_TMPFS_INTERNAL_H

#include <aosd/lock.h>
#include <aosd/types.h>

#define TMPFS_ROOT (1 << 0)

struct tmpfs_inode {
	dev_t i_dev;
	uint32_t i_links;
	uint32_t i_flags;
	mode_t i_mode;
	uint32_t i_inum;
	spinlock_t i_lock;
};

struct tmpfs_node {
	char *n_name;
	struct list_head n_subnodes;
	struct list_head n_sub;
	struct list_head n_root;
	struct tmpfs_inode *n_inode;
};

#endif
