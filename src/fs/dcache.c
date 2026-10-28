#define pr_fmt(fmt) "%s():%d " fmt, __func__, __LINE__
#include <brk/assert.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/initcall.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

static struct hlist_head dentry_htable[DENTRY_HTABLE_SIZE];
static SPINLOCK_DEFINE(dentry_htable_lock);
static struct slab_allocator dentry_cache;

void fs_dentry_cache_init(void)
{
	slab_init(&dentry_cache, sizeof(struct fs_dentry),
		  alignof(struct fs_dentry), "dentry_cache");
}
core_initcall(fs_dentry_cache_init);

static struct fs_dentry *__dentry_alloc(struct fs_super_block *sb,
					const struct qstr *name)
{
	struct fs_dentry *d;

	d = slab_alloc(&dentry_cache);
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
			slab_free(&dentry_cache, d);
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
	slab_free(&dentry_cache, dentry);
}

static struct fs_dentry *fs_dentry_alloc(struct fs_dentry *parent,
					 const struct qstr *name)
{
	struct fs_dentry *d;

	d = __dentry_alloc(parent->sb, name);
	if (!d)
		return NULL;

	d->parent = fs_dentry_get(parent);
	spin_lock(&parent->lock);
	list_add(&d->child, &parent->children);
	spin_unlock(&parent->lock);

	return d;
}

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

void fs_dentry_instantiate(struct fs_dentry *dentry, struct fs_inode *inode)
{
	/*
	 * The caller transfers one inode reference to the dentry.  Refuse to
	 * silently overwrite an existing inode (or add a second alias link):
	 * either would leak a reference and corrupt inode->dentries.
	 */
	ASSERT(dentry->inode == NULL);
	ASSERT(list_empty(&dentry->alias));

	spin_lock(&inode->lock);
	list_add(&dentry->alias, &inode->dentries);
	spin_unlock(&inode->lock);

	spin_lock(&dentry->lock);
	dentry->inode = inode;
	dentry->flags &= ~DCACHE_NEGATIVE;
	spin_unlock(&dentry->lock);
}

void fs_dentry_make_negative(struct fs_dentry *dentry)
{
	struct fs_inode *inode;
	irqstate_t flags;

	if (!dentry)
		return;

	flags = spin_lock_irqsave(&dentry->lock);
	inode = dentry->inode;
	dentry->inode = NULL;
	dentry->flags |= DCACHE_NEGATIVE;
	spin_unlock_irqrestore(&dentry->lock, flags);

	if (!inode)
		return;

	spin_lock(&inode->lock);
	list_del_init(&dentry->alias);
	spin_unlock(&inode->lock);

	fs_inode_put(inode);
}

struct fs_dentry *fs_dentry_get(struct fs_dentry *dentry)
{
	pr_debug("duplicating dentry: name=%.*s, refcnt=%d\n",
		 (int)dentry->name.len, dentry->name.name,
		 refcnt_read(&dentry->count));
	refcnt_inc(&dentry->count);
	return dentry;
}

bool fs_dentry_is_negative(struct fs_dentry *dentry)
{
	bool neg;

	spin_lock(&dentry->lock);
	neg = (dentry->flags & DCACHE_NEGATIVE) != 0;
	spin_unlock(&dentry->lock);
	return neg;
}

bool fs_dentry_is_mounted(struct fs_dentry *dentry)
{
	bool mounted;

	spin_lock(&dentry->lock);
	mounted = (dentry->flags & DCACHE_MOUNTED) != 0;
	spin_unlock(&dentry->lock);
	return mounted;
}

struct fs_inode *fs_dentry_inode(struct fs_dentry *dentry)
{
	struct fs_inode *inode;

	spin_lock(&dentry->lock);
	inode = dentry->inode;
	spin_unlock(&dentry->lock);
	return inode;
}

void fs_dentry_put(struct fs_dentry *dentry)
{
	const struct fs_dentry_ops *op;
	struct fs_inode *inode;

	pr_debug("putting dentry: name=%.*s, refcnt=%d\n",
		 (int)dentry->name.len, dentry->name.name,
		 refcnt_read(&dentry->count));

	if (refcnt_dec_fetch(&dentry->count) > 0)
		return;

	pr_debug("freeing dentry: name=%.*s\n", (int)dentry->name.len,
		 dentry->name.name);

	if (dentry->parent != dentry) {
		spin_lock(&dentry->parent->lock);
		list_del(&dentry->child);
		spin_unlock(&dentry->parent->lock);
		fs_dentry_put(dentry->parent);
	}

	if (!(dentry->flags & DCACHE_UNHASHED)) {
		pr_debug("removing dentry from hash table: name=%.*s\n",
			 (int)dentry->name.len, dentry->name.name);
		spin_lock(&dentry_htable_lock);
		ASSERT(!hlist_unhashed(&dentry->hash));
		hlist_del_init(&dentry->hash);
		spin_unlock(&dentry_htable_lock);
	}

	if (!(dentry->flags & DCACHE_NEGATIVE)) {
		inode = dentry->inode;

		spin_lock(&inode->lock);
		list_del(&dentry->alias);
		spin_unlock(&inode->lock);

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

	ASSERT(spin_is_locked_by_me(&dentry_htable_lock));

	hlist_for_each_entry(dentry, head, hash) {
		if (dentry->parent == parent &&
		    dentry->name.hash == name->hash &&
		    dentry->name.len == name->len &&
		    !dentry->ops->compare(dentry, name->len, name->name,
					  &dentry->name)) {
			/*
			 * A dentry drops its last reference before it removes
			 * itself from this hash table, so a plain refcnt_inc()
			 * could resurrect a dentry that fs_dentry_put() is
			 * already tearing down.  Only take a reference while
			 * the count is still non-zero; a dying entry is simply
			 * treated as a miss and the caller will either allocate
			 * a fresh dentry or retry the filesystem lookup.
			 */
			if (!refcnt_inc_unless_zero(&dentry->count))
				continue;
			return dentry;
		}
	}

	return NULL;
}

static void fs_dentry_hash(struct fs_dentry *dentry, u32 idx)
{
	spin_lock(&dentry->lock);
	if (dentry->parent != dentry && (dentry->flags & DCACHE_UNHASHED)) {
		dentry->flags &= ~DCACHE_UNHASHED;
		spin_lock(&dentry_htable_lock);
		hlist_add_head(&dentry->hash, &dentry_htable[idx]);
		spin_unlock(&dentry_htable_lock);
		pr_debug("added dentry to hash table: name=%.*s\n",
			 (int)dentry->name.len, dentry->name.name);
	}
	spin_unlock(&dentry->lock);
}

struct fs_dentry *fs_dentry_lookup(struct fs_dentry *parent,
				   const struct qstr *name)
{
	struct fs_inode *inode;
	struct fs_dentry *dentry, *tmp;
	struct hlist_head *head;

	u32 idx = name->hash & (DENTRY_HTABLE_SIZE - 1);
	head = &dentry_htable[idx];

	pr_debug("looking up %.*s in %.*s\n", (int)name->len, name->name,
		 (int)parent->name.len, parent->name.name);

	spin_lock(&dentry_htable_lock);
	dentry = __dentry_lookup(head, parent, name);
	spin_unlock(&dentry_htable_lock);
	if (dentry) {
		pr_debug("cache hit for %.*s\n", (int)name->len, name->name);
		return dentry;
	}
	pr_debug("cache miss for %.*s\n", (int)name->len, name->name);

	dentry = fs_dentry_alloc(parent, name);
	if (!dentry)
		return ERR_PTR(-ENOMEM);

	inode = parent->inode;
	sleeplock_acquire(&inode->rwsem);

	/*
	 * Another CPU may have populated the hash while we were allocating
	 * the new dentry.  Re-check under the directory lock so that two
	 * concurrent negative lookups converge on the same dentry instead of
	 * creating two independent negative entries.
	 */
	spin_lock(&dentry_htable_lock);
	tmp = __dentry_lookup(head, parent, name);
	spin_unlock(&dentry_htable_lock);
	if (tmp) {
		/*
		 * @dentry was never instantiated.  Mark it negative so that
		 * fs_dentry_put() does not try to iput a NULL inode.
		 */
		spin_lock(&dentry->lock);
		dentry->flags |= DCACHE_NEGATIVE;
		spin_unlock(&dentry->lock);
		sleeplock_release(&inode->rwsem);
		fs_dentry_put(dentry);
		return tmp;
	}

	tmp = inode->ops->lookup(inode, dentry, 0);
	if (IS_ERR(tmp)) {
		/*
		 * Do not cache transient lookup errors as a negative dentry.
		 * Drop the freshly allocated one and report the error.
		 */
		spin_lock(&dentry->lock);
		dentry->flags |= DCACHE_NEGATIVE;
		spin_unlock(&dentry->lock);
		sleeplock_release(&inode->rwsem);
		fs_dentry_put(dentry);
		return ERR_CAST(tmp);
	}

	if (!tmp) {
		spin_lock(&dentry->lock);
		dentry->flags |= DCACHE_NEGATIVE;
		spin_unlock(&dentry->lock);
	} else {
		dentry = tmp;
	}

	/*
	 * Publish the dentry before releasing the directory lock.  In
	 * particular a negative dentry must be visible to the next creator
	 * so that both callers operate on the same object.
	 */
	fs_dentry_hash(dentry, idx);
	sleeplock_release(&inode->rwsem);

	return dentry;
}

struct fs_dentry *fs_dentry_splice_alias(struct fs_inode *inode,
					 struct fs_dentry *dentry)
{
	struct list_head *dentries;
	struct fs_dentry *d;

	if (!inode)
		return ERR_PTR(-EINVAL);

	dentries = &inode->dentries;

	spin_lock(&inode->lock);
	list_for_each_entry(d, dentries, alias) {
		if (d->parent == dentry->parent &&
		    d->name.hash == dentry->name.hash &&
		    d->name.len == dentry->name.len &&
		    !d->ops->compare(d, dentry->name.len, dentry->name.name,
				     &d->name)) {
			/*
			 * @d is on inode->dentries, but its last reference may
			 * already have been dropped by fs_dentry_put() (the
			 * alias is unlinked under inode->lock, not before the
			 * refcount reaches zero).  Do not resurrect it.
			 */
			if (!refcnt_inc_unless_zero(&d->count))
				continue;

			spin_unlock(&inode->lock);

			/*
			 * @d already owns an inode reference.  Drop the
			 * temporary one obtained by fs_inode_get_locked() so
			 * that the alias path does not leak it.
			 */
			fs_inode_put(inode);

			dentry->flags |= DCACHE_NEGATIVE;
			fs_dentry_put(dentry);
			return d;
		}
	}
	list_add(&dentry->alias, dentries);
	spin_lock(&dentry->lock);
	dentry->inode = inode;
	spin_unlock(&dentry->lock);
	spin_unlock(&inode->lock);

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

	spin_lock(&dentry_htable_lock);
	printk("Dcache dump:\n");
	for (size_t i = 0; i < DENTRY_HTABLE_SIZE; i++) {
		head = &dentry_htable[i];
		if (hlist_empty(head))
			continue;
		printk("Bucket %zu:\n", i);
		hlist_for_each_entry(d, head, hash) {
			printk("  Name: %s, Inode: %ld, Refcnt: %d, Negative: %s, Mounted: %s\n",
			       d->name.name, d->inode ? d->inode->ino : 0,
			       refcnt_read(&d->count),
			       d->flags & DCACHE_NEGATIVE ? "true" : "false",
			       d->flags & DCACHE_MOUNTED ? "true" : "false");
		}
	}
	spin_unlock(&dentry_htable_lock);
}
