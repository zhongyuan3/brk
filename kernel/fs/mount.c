#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/hash.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>
#include <uapi/stat.h>

static struct hlist_head mount_hashtable[MOUNT_HTABLE_SIZE];
static SPINLOCK_DEFINE(mount_hashtable_lock);
static SLEEPLOCK_DEFINE(mount_lock);
static HLIST_HEAD_DEFINE(kernel_mounts);
static SPINLOCK_DEFINE(kernel_mounts_lock);

static uint32_t __mount_state_hash(struct fs_mount_state *mnt,
				   struct fs_dentry *dentry)
{
	uint32_t h1 = fnv1a_32(&mnt, sizeof(struct fs_mount_state *));
	uint32_t h2 = fnv1a_32(&dentry, sizeof(struct fs_dentry *));
	return hash_combine32(h1, h2) & (MOUNT_HTABLE_SIZE - 1);
}

static struct fs_mount_state *__mount_state_alloc(unsigned long flags)
{
	struct fs_mount_state *mnt;

	(void)flags;

	mnt = kzalloc(sizeof(struct fs_mount_state));
	if (!mnt)
		return NULL;

	mnt->parent = mnt;
	list_init(&mnt->child);
	list_init(&mnt->children);
	refcnt_init(&mnt->count, 1);
	spinlock_init(&mnt->lock, "mount_state->lock");
	hlist_node_init(&mnt->hash);
	list_init(&mnt->instance);

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
	uint32_t h = __mount_state_hash(mp_mnt, mp_dentry);

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
	uint32_t h = __mount_state_hash(mp_mnt, mp_dentry);

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
	uint32_t h = __mount_state_hash(mp_mnt, mp_dentry);
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
	struct fs_driver *driver;
	struct fs_mount_state *new_mnt;
	struct fs_path mp_path;
	int err;
	struct fs_mount_args args;
	struct fs_mount_result result;

	driver = fs_driver_lookup(type_name);
	if (!driver)
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

	args.driver = driver;
	args.dev_name = dev_name;
	args.data = data;
	args.flags = flags;
	err = driver->mount(&args, &result);
	if (err) {
		__mount_state_free(new_mnt);
		fs_path_put(&mp_path);
		return err;
	}

	new_mnt->root = result.root;
	new_mnt->sb = result.sb;

	/* Stage 3: graft vfsmount into namespace topology. */
	err = graft_tree(new_mnt, &mp_path);
	if (err) {
		fs_dentry_put(new_mnt->root);
		fs_super_block_put(new_mnt->sb);
		__mount_state_free(new_mnt);
		fs_path_put(&mp_path);
		return err;
	}

	fs_path_put(&mp_path);

	return 0;
}

int do_umount(struct fs_mount_state *mnt, int flags)
{
	struct fs_mount_state *parent;
	struct fs_dentry *mountpoint;
	struct fs_super_block *sb;
	refcnt_value_t refcnt;

	(void)flags;

	/*
	 * The VFS held one reference to the mount state, @mnt itself holds one
	 * reference, so if the reference count is greater than 2, the mount
	 * state is still in use.
	 */
	refcnt = fs_mount_state_get_refcnt(mnt);
	if (refcnt > 2)
		return -EBUSY;
	if (refcnt < 2)
		return -EINVAL;

	/*
	 * Unmount stage 1: detach from global topology while serialized by
	 * mount_lock; keep heavy put/kill callbacks out of the lock.
	 */
	sleeplock_acquire(&mount_lock);

	parent = mnt->parent;
	mountpoint = mnt->mount_point;
	sb = mnt->sb;

	/*
	 * Unpublish order for follow_mount():
	 * remove from mount hash first, then clear DCACHE_MOUNTED.
	 * This avoids false-negative windows during teardown.
	 */
	spinlock_acquire(&mount_hashtable_lock);
	hlist_del_init(&mnt->hash);
	spinlock_release(&mount_hashtable_lock);

	spinlock_acquire(&mountpoint->lock);
	mountpoint->flags &= ~DCACHE_MOUNTED;
	spinlock_release(&mountpoint->lock);

	spinlock_acquire(&parent->lock);
	list_del(&mnt->child);
	spinlock_release(&parent->lock);

	spinlock_acquire(&sb->mnt_states_lock);
	list_del(&mnt->instance);
	spinlock_release(&sb->mnt_states_lock);

	sleeplock_release(&mount_lock);

	fs_mount_state_put(parent);
	fs_dentry_put(mountpoint);

	/*
	 * Now the reference count should be 1, when the caller calls
	 * fs_mount_state_put(), the reference count will be decremented to 0,
	 * and the mount state will be freed.
	 *
	 * This is a critical point, because if the reference count is not 1,
	 * the mount state will be freed prematurely, and the filesystem will
	 * be corrupted.
	 */
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
	if (refcnt_dec_fetch(&mnt->count) > 0)
		return;

	fs_dentry_put(mnt->root);
	fs_super_block_put(mnt->sb);

	__mount_state_free(mnt);
}

struct fs_mount_state *kernel_mount(struct fs_driver *driver,
				    unsigned long flags, const char *dev_name,
				    void *data)
{
	struct fs_mount_state *new_mnt;
	int err;
	struct fs_mount_args args;
	struct fs_mount_result result;

	new_mnt = __mount_state_alloc(0);
	if (!new_mnt)
		return ERR_PTR(-ENOMEM);
	new_mnt->flags |= MOUNT_STATE_INTERNAL;

	args.driver = driver;
	args.dev_name = dev_name;
	args.data = data;
	args.flags = flags;
	err = driver->mount(&args, &result);
	if (err) {
		__mount_state_free(new_mnt);
		return ERR_PTR(err);
	}

	new_mnt->root = result.root;
	new_mnt->sb = result.sb;

	new_mnt->mount_point = result.root;
	new_mnt->parent = new_mnt;

	spinlock_acquire(&new_mnt->lock);
	list_add_tail(&new_mnt->child, &new_mnt->children);
	spinlock_release(&new_mnt->lock);

	spinlock_acquire(&kernel_mounts_lock);
	hlist_add_head(&new_mnt->hash, &kernel_mounts);
	spinlock_release(&kernel_mounts_lock);

	spinlock_acquire(&new_mnt->sb->mnt_states_lock);
	list_add(&new_mnt->instance, &new_mnt->sb->mnt_states);
	spinlock_release(&new_mnt->sb->mnt_states_lock);

	return fs_mount_state_get(new_mnt);
}

int kernel_umount(struct fs_mount_state *mnt)
{
	refcnt_value_t refcnt;

	if (!(mnt->flags & MOUNT_STATE_INTERNAL))
		return -EINVAL;

	refcnt = fs_mount_state_get_refcnt(mnt);
	if (refcnt > 2) {
		klog_warn("%s(): %p is busy: %u\n", __func__, mnt, refcnt);
		return -EBUSY;
	}
	if (refcnt < 2)
		return -EINVAL;

	spinlock_acquire(&kernel_mounts_lock);
	hlist_del_init(&mnt->hash);
	spinlock_release(&kernel_mounts_lock);

	spinlock_acquire(&mnt->parent->lock);
	list_del(&mnt->child);
	spinlock_release(&mnt->parent->lock);

	spinlock_acquire(&mnt->sb->mnt_states_lock);
	list_del(&mnt->instance);
	spinlock_release(&mnt->sb->mnt_states_lock);

	fs_mount_state_put(mnt);

	return 0;
}

refcnt_value_t fs_mount_state_get_refcnt(struct fs_mount_state *mnt)
{
	return refcnt_read(&mnt->count);
}
