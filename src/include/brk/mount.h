#ifndef BRK_MOUNT_H
#define BRK_MOUNT_H

#include <brk/bits.h>
#include <brk/fs_types.h>
#include <brk/refcnt_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

#define MOUNT_HTABLE_BITS 5
#define MOUNT_HTABLE_SIZE (1 << MOUNT_HTABLE_BITS)

#define MOUNT_STATE_INTERNAL BIT(0)

struct mount_state {
	struct mount_state *parent;
	struct list_head child;
	struct list_head children;
	struct hlist_node hash;
	struct list_head instance;
	struct dentry *mount_point;
	struct dentry *root;
	struct super_block *sb;
	refcnt_t count;
	spinlock_t lock;
	unsigned int flags;
};

struct mount_state *mount_state_lookup(const struct path *path);
struct mount_state *mount_state_get(struct mount_state *mnt);
void mount_state_put(struct mount_state *mnt);
refcnt_value_t mount_state_get_refcnt(struct mount_state *mnt);

int do_mount(const char *dev_name, const char *dir_name, const char *type,
	     unsigned long flags, void *data);
int do_umount(struct mount_state *mnt, int flags);

struct mount_state *kernel_mount(struct file_system_type *type,
				 unsigned long flags, const char *dev_name,
				 void *data);
int kernel_umount(struct mount_state *mnt);

#endif
