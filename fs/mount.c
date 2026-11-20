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
#include <uapi/errno.h>
#include <uapi/stat.h>

static struct hlist_head mount_hashtable[MOUNT_HTABLE_SIZE];
static SPINLOCK_DEFINE(mount_hashtable_lock);
static SLEEPLOCK_DEFINE(mount_lock);
static HLIST_HEAD_DEFINE(kernel_mounts);
static SPINLOCK_DEFINE(kernel_mounts_lock);

static u32 __mount_state_hash(struct mount_state *mnt, struct dentry *dentry)
{
	u32 h1 = fnv1a_32(&mnt, sizeof(struct mount_state *));
	u32 h2 = fnv1a_32(&dentry, sizeof(struct dentry *));
	return hash_combine32(h1, h2) & (MOUNT_HTABLE_SIZE - 1);
}

static struct mount_state *__mount_state_alloc(unsigned long flags)
{
	struct mount_state *mnt;

	(void)flags;

	mnt = kzalloc(sizeof(struct mount_state));
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

static void __mount_state_free(struct mount_state *mnt)
{
	kfree(mnt);
}

static bool mount_point_busy(struct mount_state *mp_mnt,
			     struct dentry *mp_dentry)
{
	struct mount_state *mnt;
	u32 h = __mount_state_hash(mp_mnt, mp_dentry);

	hlist_for_each_entry(mnt, &mount_hashtable[h], hash) {
		if (mnt->parent == mp_mnt && mnt->mount_point == mp_dentry)
			return true;
	}

	return false;
}

static int graft_tree(struct mount_state *new_mnt, struct path *mountpoint)
{
	struct dentry *mp_dentry = mountpoint->dentry;
	struct mount_state *mp_mnt = mountpoint->mnt;
	u32 h = __mount_state_hash(mp_mnt, mp_dentry);

	sleeplock_acquire(&mount_lock);

	/*
	 * Stage 1 (validation):
	 * Check mountpoint occupancy while serializing mount topology updates.
	 */
	spin_lock(&mount_hashtable_lock);
	if (mount_point_busy(mp_mnt, mp_dentry)) {
		spin_unlock(&mount_hashtable_lock);
		sleeplock_release(&mount_lock);
		return -EBUSY;
	}
	spin_unlock(&mount_hashtable_lock);

	/*
	 * Stage 2 (ownership transfer):
	 * new_mnt starts owning references to parent mount and mountpoint dentry.
	 */
	new_mnt->parent = mount_state_get(mp_mnt);
	new_mnt->mount_point = dentry_get(mp_dentry);

	/* Stage 3 (topology linkage): parent child list + global mount hash. */
	spin_lock(&mp_mnt->lock);
	list_add_tail(&new_mnt->child, &mp_mnt->children);
	spin_unlock(&mp_mnt->lock);

	/*
	 * Publish the hash entry first and only then set DCACHE_MOUNTED.
	 * follow_mount() checks the flag and then looks up the hash: if it
	 * observes the flag, the release/acquire on dentry->lock also makes
	 * the previously inserted hash entry visible.  A walker that sees the
	 * flag clear simply linearizes before the mount becomes effective.
	 *
	 * The reverse order (flag first) used to expose "flag set, hash not
	 * yet linked", making follow_mount() silently skip a mounted
	 * filesystem.
	 */
	spin_lock(&mount_hashtable_lock);
	hlist_add_head(&new_mnt->hash, &mount_hashtable[h]);
	spin_unlock(&mount_hashtable_lock);

	spin_lock(&mp_dentry->lock);
	mp_dentry->flags |= DCACHE_MOUNTED;
	spin_unlock(&mp_dentry->lock);

	/* Stage 4 (superblock mount list bookkeeping). */
	spin_lock(&new_mnt->sb->mnt_states_lock);
	list_add(&new_mnt->instance, &new_mnt->sb->mnt_states);
	spin_unlock(&new_mnt->sb->mnt_states_lock);

	sleeplock_release(&mount_lock);

	return 0;
}

struct mount_state *mount_state_lookup(const struct path *path)
{
	struct mount_state *mnt = NULL;
	struct mount_state *mp_mnt = path->mnt;
	struct dentry *mp_dentry = path->dentry;
	u32 h = __mount_state_hash(mp_mnt, mp_dentry);
	struct hlist_head *head = &mount_hashtable[h];
	spin_lock(&mount_hashtable_lock);
	hlist_for_each_entry(mnt, head, hash) {
		if (mnt->parent == mp_mnt && mnt->mount_point == mp_dentry) {
			spin_unlock(&mount_hashtable_lock);
			return mount_state_get(mnt);
		}
	}
	spin_unlock(&mount_hashtable_lock);
	return NULL;
}

int do_mount(const char *dev_name, const char *dir_name, const char *type_name,
	     unsigned long flags, void *data)
{
	struct file_system_type *type;
	struct mount_state *new_mnt;
	struct path mp_path;
	int err;
	struct mount_args args;
	struct mount_result result;

	type = file_system_type_lookup(type_name);
	if (!type)
		return -ENODEV;

	err = path_lookup(dir_name, LOOKUP_MOUNTPOINT, &mp_path);
	if (err)
		return err;

	struct inode *mp_inode = dentry_inode(mp_path.dentry);

	if (!mp_inode) {
		path_put(&mp_path);
		return -ENOENT;
	}
	if (!S_ISDIR(mp_inode->mode)) {
		path_put(&mp_path);
		return -ENOTDIR;
	}

	new_mnt = __mount_state_alloc(flags);
	if (!new_mnt) {
		path_put(&mp_path);
		return -ENOMEM;
	}

	args.type = type;
	args.dev_name = dev_name;
	args.data = data;
	args.flags = flags;
	err = type->mount(&args, &result);
	if (err) {
		__mount_state_free(new_mnt);
		path_put(&mp_path);
		return err;
	}

	new_mnt->root = result.root;
	new_mnt->sb = result.sb;

	/* Stage 3: graft vfsmount into namespace topology. */
	err = graft_tree(new_mnt, &mp_path);
	if (err) {
		dentry_put(new_mnt->root);
		super_block_put(new_mnt->sb);
		__mount_state_free(new_mnt);
		path_put(&mp_path);
		return err;
	}

	path_put(&mp_path);

	return 0;
}

int do_umount(struct mount_state *mnt, int flags)
{
	struct mount_state *parent;
	struct dentry *mountpoint;
	struct super_block *sb;
	refcnt_value_t refcnt;

	(void)flags;

	/*
	 * Unmount stage 1: detach from global topology while serialized by
	 * mount_lock; keep heavy put/kill callbacks out of the lock.
	 */
	sleeplock_acquire(&mount_lock);

	/*
	 * Revalidate and unpublish atomically.  mount_state_lookup()
	 * takes mount_hashtable_lock before taking a reference, so if the
	 * count is still 2 here (topology + caller) and the mount is still
	 * hashed, no new path can start using it after this point.  Checking
	 * the count outside the lock would allow a new lookup reference to
	 * slip in between the check and hlist_del_init().
	 */
	spin_lock(&mount_hashtable_lock);
	refcnt = mount_state_get_refcnt(mnt);
	if (hlist_unhashed(&mnt->hash) || refcnt != 2) {
		spin_unlock(&mount_hashtable_lock);
		sleeplock_release(&mount_lock);
		return refcnt > 2 ? -EBUSY : -EINVAL;
	}
	hlist_del_init(&mnt->hash);
	spin_unlock(&mount_hashtable_lock);

	parent = mnt->parent;
	mountpoint = mnt->mount_point;
	sb = mnt->sb;

	spin_lock(&mountpoint->lock);
	mountpoint->flags &= ~DCACHE_MOUNTED;
	spin_unlock(&mountpoint->lock);

	spin_lock(&parent->lock);
	list_del(&mnt->child);
	spin_unlock(&parent->lock);

	spin_lock(&sb->mnt_states_lock);
	list_del(&mnt->instance);
	spin_unlock(&sb->mnt_states_lock);

	sleeplock_release(&mount_lock);

	mount_state_put(parent);
	dentry_put(mountpoint);

	/*
	 * Now the reference count should be 1, when the caller calls
	 * mount_state_put(), the reference count will be decremented to 0,
	 * and the mount state will be freed.
	 *
	 * This is a critical point, because if the reference count is not 1,
	 * the mount state will be freed prematurely, and the filesystem will
	 * be corrupted.
	 */
	mount_state_put(mnt);

	return 0;
}

struct mount_state *mount_state_get(struct mount_state *mnt)
{
	refcnt_inc(&mnt->count);
	return mnt;
}

void mount_state_put(struct mount_state *mnt)
{
	if (refcnt_dec_fetch(&mnt->count) > 0)
		return;

	dentry_put(mnt->root);
	super_block_put(mnt->sb);

	__mount_state_free(mnt);
}

struct mount_state *kernel_mount(struct file_system_type *type,
				 unsigned long flags, const char *dev_name,
				 void *data)
{
	struct mount_state *new_mnt;
	int err;
	struct mount_args args;
	struct mount_result result;

	new_mnt = __mount_state_alloc(0);
	if (!new_mnt)
		return ERR_PTR(-ENOMEM);
	new_mnt->flags |= MOUNT_STATE_INTERNAL;

	args.type = type;
	args.dev_name = dev_name;
	args.data = data;
	args.flags = flags;
	err = type->mount(&args, &result);
	if (err) {
		__mount_state_free(new_mnt);
		return ERR_PTR(err);
	}

	new_mnt->root = result.root;
	new_mnt->sb = result.sb;

	new_mnt->mount_point = result.root;
	new_mnt->parent = new_mnt;

	spin_lock(&new_mnt->lock);
	list_add_tail(&new_mnt->child, &new_mnt->children);
	spin_unlock(&new_mnt->lock);

	spin_lock(&kernel_mounts_lock);
	hlist_add_head(&new_mnt->hash, &kernel_mounts);
	spin_unlock(&kernel_mounts_lock);

	spin_lock(&new_mnt->sb->mnt_states_lock);
	list_add(&new_mnt->instance, &new_mnt->sb->mnt_states);
	spin_unlock(&new_mnt->sb->mnt_states_lock);

	return mount_state_get(new_mnt);
}

int kernel_umount(struct mount_state *mnt)
{
	refcnt_value_t refcnt;

	if (!(mnt->flags & MOUNT_STATE_INTERNAL))
		return -EINVAL;

	refcnt = mount_state_get_refcnt(mnt);
	if (refcnt > 2) {
		pr_warn("%s(): %p is busy: %u\n", __func__, mnt, refcnt);
		return -EBUSY;
	}
	if (refcnt < 2)
		return -EINVAL;

	spin_lock(&kernel_mounts_lock);
	hlist_del_init(&mnt->hash);
	spin_unlock(&kernel_mounts_lock);

	spin_lock(&mnt->parent->lock);
	list_del(&mnt->child);
	spin_unlock(&mnt->parent->lock);

	spin_lock(&mnt->sb->mnt_states_lock);
	list_del(&mnt->instance);
	spin_unlock(&mnt->sb->mnt_states_lock);

	mount_state_put(mnt);

	return 0;
}

refcnt_value_t mount_state_get_refcnt(struct mount_state *mnt)
{
	return refcnt_read(&mnt->count);
}
