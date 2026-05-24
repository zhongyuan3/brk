#ifndef BRK_MOUNT_H
#define BRK_MOUNT_H

#include <brk/fs_types.h>
#include <brk/lock.h>
#include <brk/refcnt.h>
#include <brk/types.h>

#define MOUNT_HTABLE_BITS 5
#define MOUNT_HTABLE_SIZE (1 << MOUNT_HTABLE_BITS)

#define MNT_READONLY 0x01
#define MNT_NOSUID 0x02
#define MNT_NODEV 0x04
#define MNT_NOEXEC 0x08
#define MNT_NOATIME 0x10

struct fs_mount_state {
	/* Parent mount in namespace tree; reference held while mounted. */
	struct fs_mount_state
		*parent; /* hold reference count, self if root (no ref) */
	struct list_head child; /* protected by parent's mnt_lock */
	struct list_head children; /* protected by mnt_lock */

	struct hlist_node hash; /* protected by mount_hashtable_lock */

	struct list_head instance; /* protected by mnt_sb->mnt_states_lock */

	/* Mountpoint dentry in parent mount; reference held while mounted. */
	struct fs_dentry *mount_point;
	/* Root dentry of mounted filesystem; reference held while mounted. */
	struct fs_dentry *root;
	/* Superblock backing this mount; lifetime managed by mount lifecycle. */
	struct fs_super_block *sb;

	refcnt_t count;
	spinlock_t lock;

	unsigned int flags;
};

/*
 * Mount API contract notes
 * ------------------------
 * 1) Ownership and lifetime:
 *    - mnt_parent/mnt_mountpoint/mnt_root are owned references of struct fs_mount_state.
 *    - mnt_sb is owned by the mount and is torn down during final fs_mount_state_put()
 *      via fs_type->kill_sb().
 *    - Implementations must keep fs_mount_state_get/fs_mount_state_put and fs_path_get/fs_path_put
 *      symmetric across all success/error paths.
 *
 * 2) fs_mount_state_lookup() return semantics are intentionally simple:
 *      - returns mount with ref held
 *      - returns NULL when no mount exists at @path
 *    It should not mix "not found" with ERR_PTR() unless this header is
 *    updated to document that behavior explicitly.
 *
 * 3) Lock ordering guideline:
 *      super_block.mnt_states_lock -> mount.mnt_lock
 *    and for parent/child mount topology updates:
 *      parent->mnt_lock -> child->mnt_lock (if both are needed).
 *    Keep mount_hashtable_lock critical sections short and avoid calling
 *    teardown callbacks while holding global mount locks.
 */

struct fs_mount_state *fs_mount_state_lookup(const struct fs_path *path);
struct fs_mount_state *fs_mount_state_get(struct fs_mount_state *mnt);
void fs_mount_state_put(struct fs_mount_state *mnt);

int mount_tree_init(struct fs_path *root_path);

int do_mount(const char *dev_name, const char *dir_name, const char *type,
	     unsigned long flags, void *data);
int do_umount(struct fs_mount_state *mnt, int flags);

struct fs_mount_state *kernel_mount(struct fs_driver *fs_type,
				    unsigned long flags, const char *dev_name,
				    void *data);

#endif
