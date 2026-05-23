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

struct mount_instance {
	/* Parent mount in namespace tree; reference held while mounted. */
	struct mount_instance
		*mnt_parent; /* hold reference count, self if root (no ref) */
	struct list_head mnt_child; /* protected by parent's mnt_lock */
	struct list_head mnt_mounts; /* protected by mnt_lock */

	struct hlist_node mnt_hash; /* protected by mount_hashtable_lock */

	struct list_head mnt_instance; /* protected by mnt_sb->s_mount_lock */

	/* Mountpoint dentry in parent mount; reference held while mounted. */
	struct dentry *mnt_mountpoint;
	/* Root dentry of mounted filesystem; reference held while mounted. */
	struct dentry *mnt_root;
	/* Superblock backing this mount; lifetime managed by mount lifecycle. */
	struct super_block *mnt_sb;

	refcnt_t mnt_count;
	spinlock_t mnt_lock;

	unsigned int mnt_flags;
};

/*
 * Mount API contract notes
 * ------------------------
 * 1) Ownership and lifetime:
 *    - mnt_parent/mnt_mountpoint/mnt_root are owned references of struct mount_instance.
 *    - mnt_sb is owned by the mount and is torn down during final mount_instance_put()
 *      via fs_type->kill_sb().
 *    - Implementations must keep mount_instance_get/mount_instance_put and path_get/path_put
 *      symmetric across all success/error paths.
 *
 * 2) mount_instance_lookup() return semantics are intentionally simple:
 *      - returns mount with ref held
 *      - returns NULL when no mount exists at @path
 *    It should not mix "not found" with ERR_PTR() unless this header is
 *    updated to document that behavior explicitly.
 *
 * 3) Lock ordering guideline:
 *      super_block.s_mount_lock -> mount.mnt_lock
 *    and for parent/child mount topology updates:
 *      parent->mnt_lock -> child->mnt_lock (if both are needed).
 *    Keep mount_hashtable_lock critical sections short and avoid calling
 *    teardown callbacks while holding global mount locks.
 */

struct mount_instance *mount_instance_lookup(const struct path *path);
struct mount_instance *mount_instance_get(struct mount_instance *mnt);
void mount_instance_put(struct mount_instance *mnt);

int mount_tree_init(struct path *root_path);

int do_mount(const char *dev_name, const char *dir_name, const char *type,
	     unsigned long flags, void *data);
int do_umount(struct mount_instance *mnt, int flags);

struct mount_instance *kernel_mount(struct fs_driver *fs_type,
				    unsigned long flags, const char *dev_name,
				    void *data);

#endif
