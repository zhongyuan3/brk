#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/hash.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>
#include <uapi/stat.h>

static struct hlist_head mount_hashtable[MOUNT_HTABLE_SIZE];
static SPINLOCK_DEFINE(mount_hashtable_lock);
static SLEEPLOCK_DEFINE(mount_lock);
static struct fs_mount_state *root_mnt;

static u32 __mount_state_hash(struct fs_mount_state *mnt,
			      struct fs_dentry *dentry)
{
	u32 h1 = fnv1a_32(&mnt, sizeof(struct fs_mount_state *));
	u32 h2 = fnv1a_32(&dentry, sizeof(struct fs_dentry *));
	return hash_combine32(h1, h2) & (MOUNT_HTABLE_SIZE - 1);
}

static struct fs_mount_state *__mount_state_alloc(unsigned long flags)
{
	struct fs_mount_state *mnt;

	mnt = kzalloc(sizeof(struct fs_mount_state));
	if (!mnt)
		return NULL;

	mnt->parent = mnt;
	list_init(&mnt->child);
	list_init(&mnt->children);
	refcnt_init(&mnt->count, 1);
	spinlock_init(&mnt->lock, "mount.mnt_lock");
	hlist_node_init(&mnt->hash);
	list_init(&mnt->instance);
	mnt->flags = flags;

	return mnt;
}

static void __mount_state_free(struct fs_mount_state *mnt)
{
	kfree(mnt);
}

static bool mount_point_busy(struct fs_mount_state *mp_mnt,
			     struct fs_dentry *mp_dentry)
{
	struct fs_mount_state *mnt;
	u32 h = __mount_state_hash(mp_mnt, mp_dentry);

	hlist_for_each_entry(mnt, &mount_hashtable[h], hash) {
		if (mnt->parent == mp_mnt && mnt->mount_point == mp_dentry)
			return true;
	}

	return false;
}

static int graft_tree(struct fs_mount_state *new_mnt,
		      struct fs_path *mountpoint)
{
	struct fs_dentry *mp_dentry = mountpoint->dentry;
	struct fs_mount_state *mp_mnt = mountpoint->mnt;
	u32 h = __mount_state_hash(mp_mnt, mp_dentry);

	sleeplock_acquire(&mount_lock);

	/*
	 * Stage 1 (validation):
	 * Check mountpoint occupancy while serializing mount topology updates.
	 */
	spinlock_acquire(&mount_hashtable_lock);
	if (mount_point_busy(mp_mnt, mp_dentry)) {
		spinlock_release(&mount_hashtable_lock);
		sleeplock_release(&mount_lock);
		return -EBUSY;
	}
	spinlock_release(&mount_hashtable_lock);

	/*
	 * Stage 2 (ownership transfer):
	 * new_mnt starts owning references to parent mount and mountpoint dentry.
	 */
	new_mnt->parent = fs_mount_state_get(mp_mnt);
	new_mnt->mount_point = fs_dentry_get(mp_dentry);

	/* Stage 3 (topology linkage): parent child list + global mount hash. */
	spinlock_acquire(&mp_mnt->lock);
	list_add_tail(&new_mnt->child, &mp_mnt->children);
	spinlock_release(&mp_mnt->lock);

	/*
	 * Publish order for follow_mount():
	 * set DCACHE_MOUNTED first, then publish into mount hash.
	 * This avoids false-negative windows (mounted but bit not set).
	 */
	spinlock_acquire(&mp_dentry->lock);
	mp_dentry->flags |= DCACHE_MOUNTED;
	spinlock_release(&mp_dentry->lock);

	spinlock_acquire(&mount_hashtable_lock);
	hlist_add_head(&new_mnt->hash, &mount_hashtable[h]);
	spinlock_release(&mount_hashtable_lock);

	/* Stage 4 (superblock mount list bookkeeping). */
	spinlock_acquire(&new_mnt->sb->mnt_states_lock);
	list_add(&new_mnt->instance, &new_mnt->sb->mnt_states);
	spinlock_release(&new_mnt->sb->mnt_states_lock);

	sleeplock_release(&mount_lock);

	return 0;
}

struct fs_mount_state *fs_mount_state_lookup(const struct fs_path *path)
{
	struct fs_mount_state *mnt = NULL;
	struct fs_mount_state *mp_mnt = path->mnt;
	struct fs_dentry *mp_dentry = path->dentry;
	u32 h = __mount_state_hash(mp_mnt, mp_dentry);
	struct hlist_head *head = &mount_hashtable[h];
	spinlock_acquire(&mount_hashtable_lock);
	hlist_for_each_entry(mnt, head, hash) {
		if (mnt->parent == mp_mnt && mnt->mount_point == mp_dentry) {
			spinlock_release(&mount_hashtable_lock);
			return fs_mount_state_get(mnt);
		}
	}
	spinlock_release(&mount_hashtable_lock);
	return NULL;
}

int do_mount(const char *dev_name, const char *dir_name, const char *type_name,
	     unsigned long flags, void *data)
{
	struct fs_driver *type;
	struct fs_mount_state *new_mnt;
	struct fs_path mp_path;
	struct fs_dentry *root_dentry;
	int err;

	type = fs_driver_lookup(type_name);
	if (!type)
		return -ENODEV;

	err = fs_path_lookup(dir_name, 0, &mp_path);
	if (err)
		return err;

	if (!S_ISDIR(mp_path.dentry->inode->mode)) {
		fs_path_put(&mp_path);
		return -ENOTDIR;
	}

	new_mnt = __mount_state_alloc(flags);
	if (!new_mnt) {
		fs_path_put(&mp_path);
		return -ENOMEM;
	}

	root_dentry = type->mount(type, flags, dev_name, data);
	if (IS_ERR(root_dentry)) {
		err = PTR_ERR(root_dentry);
		__mount_state_free(new_mnt);
		fs_path_put(&mp_path);
		return err;
	}

	new_mnt->root = root_dentry;
	new_mnt->sb = root_dentry->sb;

	/* Stage 3: graft vfsmount into namespace topology. */
	err = graft_tree(new_mnt, &mp_path);
	if (err) {
		/*
		 * type->mount() returns the root dentry by move semantics.
		 * graft failure means mount not published, so drop mnt_root ref
		 * explicitly before tearing down the superblock.
		 */
		fs_dentry_put(new_mnt->root);
		type->kill_sb(new_mnt->sb);
		__mount_state_free(new_mnt);
		fs_path_put(&mp_path);
		return err;
	}

	fs_path_put(&mp_path);

	return 0;
}

int do_umount(struct fs_mount_state *mnt, int flags)
{
	(void)flags;
	fs_mount_state_put(mnt);
	return 0;
}

struct fs_mount_state *fs_mount_state_get(struct fs_mount_state *mnt)
{
	refcnt_inc(&mnt->count);
	return mnt;
}

void fs_mount_state_put(struct fs_mount_state *mnt)
{
	struct fs_mount_state *parent;
	struct fs_dentry *mountpoint;
	struct fs_dentry *root;
	struct fs_super_block *sb;

	if (refcnt_dec_fetch(&mnt->count) > 0)
		return;

	/*
	 * Unmount stage 1: detach from global topology while serialized by
	 * mount_lock; keep heavy put/kill callbacks out of the lock.
	 */
	sleeplock_acquire(&mount_lock);

	parent = mnt->parent;
	mountpoint = mnt->mount_point;
	root = mnt->root;
	sb = mnt->sb;

	/*
	 * Unpublish order for follow_mount():
	 * remove from mount hash first, then clear DCACHE_MOUNTED.
	 * This avoids false-negative windows during teardown.
	 */
	spinlock_acquire(&mount_hashtable_lock);
	hlist_del_init(&mnt->hash);
	spinlock_release(&mount_hashtable_lock);

	spinlock_acquire(&mnt->mount_point->lock);
	mnt->mount_point->flags &= ~DCACHE_MOUNTED;
	spinlock_release(&mnt->mount_point->lock);

	spinlock_acquire(&mnt->parent->lock);
	list_del(&mnt->child);
	spinlock_release(&mnt->parent->lock);

	spinlock_acquire(&mnt->sb->mnt_states_lock);
	list_del(&mnt->instance);
	spinlock_release(&mnt->sb->mnt_states_lock);

	sleeplock_release(&mount_lock);

	/* Unmount stage 2: release owned references and tear down sb. */
	fs_mount_state_put(parent);
	fs_dentry_put(mountpoint);
	fs_dentry_put(root);
	sb->driver->kill_sb(sb);

	__mount_state_free(mnt);
}

struct fs_mount_state *kernel_mount(struct fs_driver *fs_type,
				    unsigned long flags, const char *dev_name,
				    void *data)
{
	struct fs_mount_state *new_mnt;
	struct fs_dentry *root_dentry;

	new_mnt = __mount_state_alloc(0);
	if (!new_mnt)
		return ERR_PTR(-ENOMEM);

	root_dentry = fs_type->mount(fs_type, flags, dev_name, data);
	if (IS_ERR(root_dentry)) {
		__mount_state_free(new_mnt);
		return ERR_CAST(root_dentry);
	}

	new_mnt->mount_point = root_dentry;
	new_mnt->root = root_dentry;
	new_mnt->sb = root_dentry->sb;

	return new_mnt;
}

int mount_tree_init(struct fs_path *root_path)
{
	struct fs_mount_state *mnt = kernel_mount(&tmpfs_fs_type, 0, "", NULL);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);
	root_mnt = mnt;
	root_path->mnt = fs_mount_state_get(root_mnt);
	root_path->dentry = fs_dentry_get(root_mnt->root);
	return 0;
}
