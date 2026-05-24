#define KLOG_LEVEL KLOG_INFO
#include <brk/assert.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>

static struct hlist_head dentry_htable[DENTRY_HTABLE_SIZE];
static SPINLOCK_DEFINE(dentry_htable_lock);
static struct kobj_pool dentry_cache;

void fs_dentry_cache_init(void)
{
	kobj_pool_init(&dentry_cache, sizeof(struct fs_dentry),
		       alignof(struct fs_dentry), "dentry_cache");
}

static struct fs_dentry *__dentry_alloc(struct fs_super_block *sb,
					const struct qstr *name)
{
	struct fs_dentry *d;

	d = kobj_pool_alloc(&dentry_cache);
	if (!d)
		return NULL;

	refcnt_init(&d->count, 1);
	d->flags = DCACHE_UNHASHED;
	spinlock_init(&d->lock, "dentry.d_lock");
	d->inode = NULL;
	list_init(&d->alias);
	d->parent = d;
	list_init(&d->child);
	list_init(&d->children);
	hlist_node_init(&d->hash);
	if (name->len < DENTRY_SHORT_NAME_SIZE) {
		d->name.name = d->short_name;
		d->short_name[DENTRY_SHORT_NAME_SIZE - 1] = '\0';
		memcpy(d->short_name, name->name, name->len);
		d->short_name[name->len] = '\0';
	} else {
		d->long_name = kmalloc(name->len + 1);
		if (!d->long_name) {
			kobj_pool_free(&dentry_cache, d);
			return NULL;
		}
		d->name.name = d->long_name;
		memcpy(d->long_name, name->name, name->len);
		d->long_name[name->len] = '\0';
	}
	d->name.len = name->len;
	d->name.hash = name->hash;
	d->sb = sb;
	d->ops = sb->default_dops;
	d->private_data = NULL;
	return d;
}

static void __dentry_free(struct fs_dentry *dentry)
{
	if (dentry->name.len >= DENTRY_SHORT_NAME_SIZE)
		kfree(dentry->long_name);
	kobj_pool_free(&dentry_cache, dentry);
}

static struct fs_dentry *fs_dentry_alloc(struct fs_dentry *parent,
					 const struct qstr *name)
{
	struct fs_dentry *d;

	d = __dentry_alloc(parent->sb, name);
	if (!d)
		return NULL;

	d->parent = fs_dentry_get(parent);
	spinlock_acquire(&parent->lock);
	list_add(&d->child, &parent->children);
	spinlock_release(&parent->lock);

	return d;
}

/**
  * fs_dentry_make_root() - Make a root dentry
  * @root_inode: root inode
  *
  * Only used for file system mounting operations.
  *
  * Return: The root dentry or %NULL.
  */
struct fs_dentry *fs_dentry_make_root(struct fs_inode *root_inode)
{
	struct fs_dentry *d;

	if (!root_inode)
		return NULL;

	d = __dentry_alloc(root_inode->sb, &slash_name);
	if (!d)
		return NULL;

	fs_dentry_instantiate(d, root_inode);

	return d;
}

/**
 * fs_dentry_alloc_anon() - Allocate an unhashed positive dentry for a given inode
 * @inode: inode to attach (caller holds inode lifetime)
 * @name: dentry name (for debugging / generic compare)
 *
 * Used for kernel objects such as pipes that are not reachable by path lookup.
 */
struct fs_dentry *fs_dentry_alloc_anon(struct fs_inode *inode,
				       const struct qstr *name)
{
	struct fs_dentry *d;

	d = __dentry_alloc(inode->sb, name);
	if (!d)
		return NULL;

	fs_dentry_instantiate(d, inode);
	return d;
}

/**
 * fs_dentry_instantiate() - Attach @inode to @dentry
 * @dentry: dentry to attach
 * @inode: inode to attach to this dentry
 */
void fs_dentry_instantiate(struct fs_dentry *dentry, struct fs_inode *inode)
{
	dentry->inode = inode;
	spinlock_acquire(&inode->lock);
	list_add(&dentry->alias, &inode->dentries);
	spinlock_release(&inode->lock);

	spinlock_acquire(&dentry->lock);
	dentry->flags &= ~DCACHE_NEGATIVE;
	spinlock_release(&dentry->lock);
}

/* Increment the reference count of @dentry */
struct fs_dentry *fs_dentry_get(struct fs_dentry *dentry)
{
	klog_debug(
		"%s(): Duplicating dentry: name=%.*s, inode=%ld, refcnt=%d\n",
		__func__, dentry->name.len, dentry->name.name,
		dentry->inode->ino, arc_get(&dentry->count));
	refcnt_inc(&dentry->count);
	return dentry;
}

/* Decrement the reference count of @dentry */
void fs_dentry_put(struct fs_dentry *dentry)
{
	const struct fs_dentry_ops *op;
	struct fs_inode *inode;

	klog_debug("%s(): Putting dentry: name=%.*s, inode=%ld, refcnt=%d\n",
		   __func__, dentry->name.len, dentry->name.name,
		   dentry->inode->ino, arc_get(&dentry->count));

	if (refcnt_dec_fetch(&dentry->count) > 0)
		return;

	klog_debug("%s(): Freeing dentry: name=%.*s, inode=%ld\n", __func__,
		   dentry->name.len, dentry->name.name, dentry->inode->ino);

	if (dentry->parent != dentry) {
		spinlock_acquire(&dentry->parent->lock);
		list_del(&dentry->child);
		spinlock_release(&dentry->parent->lock);
		fs_dentry_put(dentry->parent);
	}

	if (!(dentry->flags & DCACHE_UNHASHED)) {
		klog_debug(
			"%s(): Removing dentry from hash table: name=%.*s, inode=%ld\n",
			__func__, dentry->name.len, dentry->name.name,
			dentry->inode->ino);
		spinlock_acquire(&dentry_htable_lock);
		ASSERT(!hlist_unhashed(&dentry->hash));
		hlist_del_init(&dentry->hash);
		spinlock_release(&dentry_htable_lock);
	}

	if (!(dentry->flags & DCACHE_NEGATIVE)) {
		inode = dentry->inode;

		spinlock_acquire(&inode->lock);
		list_del(&dentry->alias);
		spinlock_release(&inode->lock);

		op = dentry->ops;
		if (op->release)
			op->release(dentry);

		if (op->iput)
			op->iput(dentry, inode);
		else
			fs_inode_put(inode);
	}

	__dentry_free(dentry);
}

static struct fs_dentry *__dentry_lookup(struct hlist_head *head,
					 struct fs_dentry *parent,
					 const struct qstr *name)
{
	struct fs_dentry *dentry;

	ASSERT(spinlock_holding(&dentry_htable_lock));

	hlist_for_each_entry(dentry, head, hash) {
		if (dentry->parent == parent &&
		    dentry->name.hash == name->hash &&
		    dentry->name.len == name->len &&
		    !dentry->ops->compare(dentry, name->len, name->name,
					  &dentry->name)) {
			refcnt_inc(&dentry->count);
			return dentry;
		}
	}

	return NULL;
}

/**
 * fs_dentry_lookup() - Resolve one path component under @parent
 * @parent: parent dentry
 * @name: component name with hash prepared by caller
 *
 * Lookup first checks the dcache hash. On miss, it allocates a child dentry and
 * invokes parent inode ->lookup(). The returned dentry may be positive (existing)
 * or negative (DCACHE_NEGATIVE set, indicating not found).
 *
 * Return: The dentry on success or ERR_PTR(-errno) on failure.
 */
struct fs_dentry *fs_dentry_lookup(struct fs_dentry *parent,
				   const struct qstr *name)
{
	struct fs_inode *inode;
	struct fs_dentry *dentry, *tmp;
	struct hlist_head *head;

	u32 idx = name->hash & (DENTRY_HTABLE_SIZE - 1);
	head = &dentry_htable[idx];

	klog_debug("%s(): Looking up %.*s in %.*s\n", __func__, name->len,
		   name->name, parent->name.len, parent->name.name);

	spinlock_acquire(&dentry_htable_lock);
	dentry = __dentry_lookup(head, parent, name);
	spinlock_release(&dentry_htable_lock);
	if (dentry) {
		klog_debug("%s(): Cache hit for %.*s\n", __func__, name->len,
			   name->name);
		return dentry;
	}
	klog_debug("%s(): Cache miss for %.*s\n", __func__, name->len,
		   name->name);

	dentry = fs_dentry_alloc(parent, name);
	if (!dentry)
		return ERR_PTR(-ENOMEM);

	inode = parent->inode;
	sleeplock_acquire(&inode->rwsem);
	tmp = inode->ops->lookup(inode, dentry, 0);
	sleeplock_release(&inode->rwsem);
	if (IS_ERR(tmp)) {
		dentry->flags |= DCACHE_NEGATIVE;
		fs_dentry_put(dentry);
		return ERR_CAST(tmp);
	}

	if (!tmp)
		dentry->flags |= DCACHE_NEGATIVE;
	else
		dentry = tmp;

	spinlock_acquire(&dentry->lock);
	if (dentry->parent != dentry && dentry->flags & DCACHE_UNHASHED) {
		dentry->flags &= ~DCACHE_UNHASHED;
		spinlock_acquire(&dentry_htable_lock);
		hlist_add_head(&dentry->hash, &dentry_htable[idx]);
		spinlock_release(&dentry_htable_lock);
		spinlock_release(&dentry->lock);
		klog_debug(
			"%s(): Added dentry to hash table: name=%.*s, inode=%ld\n",
			__func__, dentry->name.len, dentry->name.name,
			dentry->inode->ino);
	} else {
		spinlock_release(&dentry->lock);
		klog_debug(
			"%s(): Dentry already in hash table: name=%.*s, inode=%ld\n",
			__func__, dentry->name.len, dentry->name.name,
			dentry->inode->ino);
	}

	return dentry;
}

/**
 * fs_dentry_splice_alias() - Attach @inode to @dentry or reuse existing alias
 * @inode: inode to splice the dentry into
 * @dentry: dentry to splice
 *
 * Return: The alias dentry on success or ERR_PTR(-errno) on failure.
 */
struct fs_dentry *fs_dentry_splice_alias(struct fs_inode *inode,
					 struct fs_dentry *dentry)
{
	struct list_head *dentries;
	struct fs_dentry *d;

	if (!inode)
		return ERR_PTR(-EINVAL);

	dentries = &inode->dentries;

	spinlock_acquire(&inode->lock);
	list_for_each_entry(d, dentries, alias) {
		if (d->parent == dentry->parent &&
		    d->name.hash == dentry->name.hash &&
		    d->name.len == dentry->name.len &&
		    !d->ops->compare(d, dentry->name.len, dentry->name.name,
				     &d->name)) {
			spinlock_release(&inode->lock);
			fs_dentry_get(d);
			dentry->flags |= DCACHE_NEGATIVE;
			fs_dentry_put(dentry);
			return d;
		}
	}
	list_add(&dentry->alias, dentries);
	dentry->inode = inode;
	spinlock_release(&inode->lock);

	return dentry;
}

const struct qstr slash_name = {
	.name = "/",
	.len = 1,
	.hash = SLASH_NAME_HASH,
};
const struct qstr dot_name = {
	.name = ".",
	.len = 1,
	.hash = DOT_NAME_HASH,
};
const struct qstr dotdot_name = {
	.name = "..",
	.len = 2,
	.hash = DOTDOT_NAME_HASH,
};

static int generic_compare(const struct fs_dentry *dentry, unsigned int len,
			   const char *str, const struct qstr *name)
{
	(void)dentry;
	if (len > name->len)
		return 1;
	if (len < name->len)
		return -1;
	return memcmp(str, name->name, len);
}

const struct fs_dentry_ops generic_dop = {
	.compare = generic_compare,
};

void fs_dcache_dump(void)
{
	struct fs_dentry *d;
	struct hlist_head *head;

	spinlock_acquire(&dentry_htable_lock);
	printk("Dcache dump:\n");
	for (usize_t i = 0; i < DENTRY_HTABLE_SIZE; i++) {
		head = &dentry_htable[i];
		if (hlist_empty(head))
			continue;
		printk("Bucket %zu:\n", i);
		hlist_for_each_entry(d, head, hash) {
			printk("  Name: %s, Inode: %ld, Refcnt: %d, Negative: %s, Mounted: %s\n",
			       d->name.name, d->inode->ino,
			       refcnt_read(&d->count),
			       d->flags & DCACHE_NEGATIVE ? "true" : "false",
			       d->flags & DCACHE_MOUNTED ? "true" : "false");
		}
	}
	spinlock_release(&dentry_htable_lock);
}
