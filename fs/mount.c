/*
 * mount.c - Mount mechanism core: graft_tree
 *
 * "Graft" the directory tree of a new file system onto a mount point in the original directory tree.
 *
 * State before mounting:
 *
 *   root_mnt (rootfs)
 *     mnt_root  ──▶  root_dentry "/"
 *                        └── dentry "/mnt"  (DCACHE_MOUNTED not set)
 *
 * State after mounting:
 *
 *   root_mnt (rootfs)
 *     mnt_root  ──▶  root_dentry "/"
 *                        └── dentry "/mnt"  (DCACHE_MOUNTED set)
 *                                │
 *                    ┌───────────┘ lookup_mount() finds via global hash table ──▶
 *                    │
 *                    ▼
 *              new_mnt (ext4)
 *                mnt_mountpoint ──▶ dentry "/mnt"  (mount point in original tree)
 *                mnt_root       ──▶ dentry "/"     (ext4's own root)
 *                mnt_parent     ──▶ root_mnt
 */
#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/hash.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/stat.h>
#include <brk/string.h>
#include <brk/types.h>

static struct hlist_head mount_hashtable[MOUNT_HTABLE_SIZE];
static SPINLOCK_DEFINE(mount_hashtable_lock);
static SLEEPLOCK_DEFINE(mount_lock);
static struct mount *root_mnt;

static u32 hash(struct mount *mnt, struct dentry *dentry)
{
	u32 h1 = fnv1a_32(&mnt, sizeof(struct mount *));
	u32 h2 = fnv1a_32(&dentry, sizeof(struct dentry *));
	return hash_combine32(h1, h2) & (MOUNT_HTABLE_SIZE - 1);
}

static struct mount *alloc_mount(unsigned long flags)
{
	struct mount *mnt;

	mnt = kzalloc(sizeof(struct mount));
	if (!mnt)
		return NULL;

	mnt->mnt_parent = mnt;
	list_init(&mnt->mnt_child);
	list_init(&mnt->mnt_mounts);
	refcnt_init(&mnt->mnt_count, 1);
	spinlock_init(&mnt->mnt_lock, "mount.mnt_lock");
	hlist_node_init(&mnt->mnt_hash);
	list_init(&mnt->mnt_instance);
	mnt->mnt_flags = flags;

	return mnt;
}

static void free_mount(struct mount *mnt)
{
	kfree(mnt);
}

static bool mountpoint_busy(struct mount *mp_mnt, struct dentry *mp_dentry)
{
	struct mount *mnt;
	u32 h = hash(mp_mnt, mp_dentry);

	hlist_for_each_entry(mnt, &mount_hashtable[h], mnt_hash) {
		if (mnt->mnt_parent == mp_mnt &&
		    mnt->mnt_mountpoint == mp_dentry)
			return true;
	}

	return false;
}

static int graft_tree(struct mount *new_mnt, struct path *mountpoint)
{
	struct dentry *mp_dentry = mountpoint->dentry;
	struct mount *mp_mnt = mountpoint->mnt;
	u32 h = hash(mp_mnt, mp_dentry);

	sleeplock_acquire(&mount_lock);

	/*
	 * Stage 1 (validation):
	 * Check mountpoint occupancy while serializing mount topology updates.
	 */
	spinlock_acquire(&mount_hashtable_lock);
	if (mountpoint_busy(mp_mnt, mp_dentry)) {
		spinlock_release(&mount_hashtable_lock);
		sleeplock_release(&mount_lock);
		return -EBUSY;
	}
	spinlock_release(&mount_hashtable_lock);

	/*
	 * Stage 2 (ownership transfer):
	 * new_mnt starts owning references to parent mount and mountpoint dentry.
	 */
	new_mnt->mnt_parent = mount_dup(mp_mnt);
	new_mnt->mnt_mountpoint = dentry_dup(mp_dentry);

	/* Stage 3 (topology linkage): parent child list + global mount hash. */
	spinlock_acquire(&mp_mnt->mnt_lock);
	list_add_tail(&new_mnt->mnt_child, &mp_mnt->mnt_mounts);
	spinlock_release(&mp_mnt->mnt_lock);

	/*
	 * Publish order for follow_mount():
	 * set DCACHE_MOUNTED first, then publish into mount hash.
	 * This avoids false-negative windows (mounted but bit not set).
	 */
	spinlock_acquire(&mp_dentry->d_lock);
	mp_dentry->d_flags |= DCACHE_MOUNTED;
	spinlock_release(&mp_dentry->d_lock);

	spinlock_acquire(&mount_hashtable_lock);
	hlist_add_head(&new_mnt->mnt_hash, &mount_hashtable[h]);
	spinlock_release(&mount_hashtable_lock);

	/* Stage 4 (superblock mount list bookkeeping). */
	spinlock_acquire(&new_mnt->mnt_sb->s_mount_lock);
	list_add(&new_mnt->mnt_instance, &new_mnt->mnt_sb->s_mounts);
	spinlock_release(&new_mnt->mnt_sb->s_mount_lock);

	sleeplock_release(&mount_lock);

	return 0;
}

/**
 * lookup_mount() - Resolve child mount mounted on @path
 * @path: Current path (mnt + dentry)
 *
 * Return: mount with reference held, or %NULL if no child mount exists.
 */
struct mount *lookup_mount(const struct path *path)
{
	struct mount *mnt = NULL;
	struct mount *mp_mnt = path->mnt;
	struct dentry *mp_dentry = path->dentry;
	u32 h = hash(mp_mnt, mp_dentry);
	struct hlist_head *head = &mount_hashtable[h];
	spinlock_acquire(&mount_hashtable_lock);
	hlist_for_each_entry(mnt, head, mnt_hash) {
		if (mnt->mnt_parent == mp_mnt &&
		    mnt->mnt_mountpoint == mp_dentry) {
			spinlock_release(&mount_hashtable_lock);
			return mount_dup(mnt);
		}
	}
	spinlock_release(&mount_hashtable_lock);
	return NULL;
}

/**
 * do_mount() - Top-level function for mounting file systems
 * @dev_name: Device path or special name
 * @dir_name: Mount point path (user space string)
 * @type_name: File system type name
 * @flags: MS_* mount flags
 * @data: File system private mount options
 *
 * Return: %0 on success, negative errno on failure.
 */
int do_mount(const char *dev_name, const char *dir_name, const char *type_name,
	     unsigned long flags, void *data)
{
	struct file_system_type *type;
	struct mount *new_mnt;
	struct path mp_path;
	struct dentry *root_dentry;
	int err;

	type = get_filesystem(type_name);
	if (!type)
		return -ENODEV;

	err = path_lookup(dir_name, 0, &mp_path);
	if (err)
		return err;

	if (!S_ISDIR(mp_path.dentry->d_inode->i_mode)) {
		path_put(&mp_path);
		return -ENOTDIR;
	}

	new_mnt = alloc_mount(flags);
	if (!new_mnt) {
		path_put(&mp_path);
		return -ENOMEM;
	}

	root_dentry = type->mount(type, flags, dev_name, data);
	if (IS_ERR(root_dentry)) {
		err = PTR_ERR(root_dentry);
		free_mount(new_mnt);
		path_put(&mp_path);
		return err;
	}

	new_mnt->mnt_root = root_dentry;
	new_mnt->mnt_sb = root_dentry->d_sb;

	/* Stage 3: graft vfsmount into namespace topology. */
	err = graft_tree(new_mnt, &mp_path);
	if (err) {
		/*
		 * type->mount() returns the root dentry by move semantics.
		 * graft failure means mount not published, so drop mnt_root ref
		 * explicitly before tearing down the superblock.
		 */
		dentry_put(new_mnt->mnt_root);
		type->kill_sb(new_mnt->mnt_sb);
		free_mount(new_mnt);
		path_put(&mp_path);
		return err;
	}

	path_put(&mp_path);

	return 0;
}

/**
 * do_umount() - Top-level function for unmounting file systems
 * @mnt: Mount to unmount
 * @flags: Unmount flags
 *
 * Return: %0 on success, negative errno on failure.
 */
int do_umount(struct mount *mnt, int flags)
{
	(void)flags;
	mount_put(mnt);
	return 0;
}

/* Returns @mnt with refcount incremented. */
struct mount *mount_dup(struct mount *mnt)
{
	refcnt_inc(&mnt->mnt_count);
	return mnt;
}

/* Drops one reference and may tear down mount at zero. */
void mount_put(struct mount *mnt)
{
	struct mount *parent;
	struct dentry *mountpoint;
	struct dentry *root;
	struct super_block *sb;

	if (refcnt_dec_fetch(&mnt->mnt_count) > 0)
		return;

	/*
	 * Unmount stage 1: detach from global topology while serialized by
	 * mount_lock; keep heavy put/kill callbacks out of the lock.
	 */
	sleeplock_acquire(&mount_lock);

	parent = mnt->mnt_parent;
	mountpoint = mnt->mnt_mountpoint;
	root = mnt->mnt_root;
	sb = mnt->mnt_sb;

	/*
	 * Unpublish order for follow_mount():
	 * remove from mount hash first, then clear DCACHE_MOUNTED.
	 * This avoids false-negative windows during teardown.
	 */
	spinlock_acquire(&mount_hashtable_lock);
	hlist_del_init(&mnt->mnt_hash);
	spinlock_release(&mount_hashtable_lock);

	spinlock_acquire(&mnt->mnt_mountpoint->d_lock);
	mnt->mnt_mountpoint->d_flags &= ~DCACHE_MOUNTED;
	spinlock_release(&mnt->mnt_mountpoint->d_lock);

	spinlock_acquire(&mnt->mnt_parent->mnt_lock);
	list_del(&mnt->mnt_child);
	spinlock_release(&mnt->mnt_parent->mnt_lock);

	spinlock_acquire(&mnt->mnt_sb->s_mount_lock);
	list_del(&mnt->mnt_instance);
	spinlock_release(&mnt->mnt_sb->s_mount_lock);

	sleeplock_release(&mount_lock);

	/* Unmount stage 2: release owned references and tear down sb. */
	mount_put(parent);
	dentry_put(mountpoint);
	dentry_put(root);
	sb->s_type->kill_sb(sb);

	free_mount(mnt);
}

struct mount *kernel_mount(struct file_system_type *fs_type,
			   unsigned long flags, const char *dev_name,
			   void *data)
{
	struct mount *new_mnt;
	struct dentry *root_dentry;

	new_mnt = alloc_mount(0);
	if (!new_mnt)
		return ERR_PTR(-ENOMEM);

	root_dentry = fs_type->mount(fs_type, flags, dev_name, data);
	if (IS_ERR(root_dentry)) {
		free_mount(new_mnt);
		return ERR_CAST(root_dentry);
	}

	new_mnt->mnt_mountpoint = root_dentry;
	new_mnt->mnt_root = root_dentry;
	new_mnt->mnt_sb = root_dentry->d_sb;

	return new_mnt;
}

int init_mount_tree(struct path *root_path)
{
	struct mount *mnt = kernel_mount(&tmpfs_fs_type, 0, "", NULL);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);
	root_mnt = mnt;
	root_path->mnt = mount_dup(root_mnt);
	root_path->dentry = dentry_dup(root_mnt->mnt_root);
	return 0;
}
