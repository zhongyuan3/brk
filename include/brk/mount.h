#ifndef BRK_MOUNT_H
#define BRK_MOUNT_H

#include <brk/bits.h>
#include <brk/fs_types.h>
#include <brk/lock.h>
#include <brk/refcnt.h>
#include <brk/types.h>

#define MOUNT_HTABLE_BITS 5
#define MOUNT_HTABLE_SIZE (1 << MOUNT_HTABLE_BITS)

#define MOUNT_STATE_INTERNAL BIT(0)

struct fs_mount_state {
	struct fs_mount_state *parent;
	struct list_head child;
	struct list_head children;
	struct hlist_node hash;
	struct list_head instance;
	struct fs_dentry *mount_point;
	struct fs_dentry *root;
	struct fs_super_block *sb;
	refcnt_t count;
	spinlock_t lock;
	unsigned int flags;
};

struct fs_mount_state *fs_mount_state_lookup(const struct fs_path *path);
struct fs_mount_state *fs_mount_state_get(struct fs_mount_state *mnt);
void fs_mount_state_put(struct fs_mount_state *mnt);
refcnt_value_t fs_mount_state_get_refcnt(struct fs_mount_state *mnt);

int do_mount(const char *dev_name, const char *dir_name, const char *type,
	     unsigned long flags, void *data);
int do_umount(struct fs_mount_state *mnt, int flags);

struct fs_mount_state *kernel_mount(struct fs_driver *driver,
				    unsigned long flags, const char *dev_name,
				    void *data);
int kernel_umount(struct fs_mount_state *mnt);

#endif
