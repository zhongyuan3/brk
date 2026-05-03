#include <brk/align.h>
#include <brk/assert.h>
#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>

static struct hlist_head dentry_htable[DENTRY_HTABLE_SIZE];
static SPINLOCK_DEFINE(dentry_htable_lock);
static struct kmem_cache dentry_cache;

void dentry_cache_init(void)
{
	kmem_cache_init(&dentry_cache, sizeof(struct dentry),
			alignof(struct dentry), "dentry_cache");
}

static struct dentry *__alloc_dentry(struct super_block *sb,
				     const struct qstr *name)
{
	struct dentry *d;

	d = kmem_cache_alloc(&dentry_cache);
	if (!d)
		return NULL;

	arc_init(&d->d_count, 1);
	d->d_flags = DCACHE_UNHASHED;
	spinlock_init(&d->d_lock, "dentry.d_lock");
	d->d_inode = NULL;
	list_init(&d->d_alias);
	d->d_parent = d;
	list_init(&d->d_child);
	list_init(&d->d_subdirs);
	hlist_node_init(&d->d_hash);
	if (name->len < DENTRY_SHORT_NAME_SIZE) {
		d->d_name.name = d->d_short_name;
		d->d_short_name[DENTRY_SHORT_NAME_SIZE - 1] = '\0';
		memcpy(d->d_short_name, name->name, name->len);
		d->d_short_name[name->len] = '\0';
	} else {
		d->d_long_name = kmalloc(name->len + 1);
		if (!d->d_long_name) {
			kmem_cache_free(&dentry_cache, d);
			return NULL;
		}
		d->d_name.name = d->d_long_name;
		memcpy(d->d_long_name, name->name, name->len);
		d->d_long_name[name->len] = '\0';
	}
	d->d_name.len = name->len;
	d->d_name.hash = name->hash;
	d->d_sb = sb;
	d->d_op = sb->s_d_op;
	d->d_fsdata = NULL;
	return d;
}

static void __free_dentry(struct dentry *dentry)
{
	if (dentry->d_name.len >= DENTRY_SHORT_NAME_SIZE)
		kfree(dentry->d_long_name);
	kmem_cache_free(&dentry_cache, dentry);
}

static struct dentry *alloc_dentry(struct dentry *parent,
				   const struct qstr *name)
{
	struct dentry *d;

	d = __alloc_dentry(parent->d_sb, name);
	if (!d)
		return NULL;

	d->d_parent = dentry_dup(parent);
	spinlock_acquire(&parent->d_lock);
	list_add(&d->d_child, &parent->d_subdirs);
	spinlock_release(&parent->d_lock);

	return d;
}

/**
  * dentry_make_root() - Make a root dentry
  * @root_inode: root inode
  *
  * Only used for file system mounting operations.
  *
  * Return: The root dentry or %NULL.
  */
struct dentry *dentry_make_root(struct inode *root_inode)
{
	struct dentry *d;

	if (!root_inode)
		return NULL;

	d = __alloc_dentry(root_inode->i_sb, &slash_name);
	if (!d)
		return NULL;

	dentry_instantiate(d, root_inode);

	return d;
}

/**
 * dentry_alloc_anon() - Allocate an unhashed positive dentry for a given inode
 * @inode: inode to attach (caller holds inode lifetime)
 * @name: dentry name (for debugging / generic compare)
 *
 * Used for kernel objects such as pipes that are not reachable by path lookup.
 */
struct dentry *dentry_alloc_anon(struct inode *inode, const struct qstr *name)
{
	struct dentry *d;

	d = __alloc_dentry(inode->i_sb, name);
	if (!d)
		return NULL;

	dentry_instantiate(d, inode);
	return d;
}

/**
 * dentry_instantiate() - Attach @inode to @dentry
 * @dentry: dentry to attach
 * @inode: inode to attach to this dentry
 */
void dentry_instantiate(struct dentry *dentry, struct inode *inode)
{
	dentry->d_inode = inode;
	spinlock_acquire(&inode->i_lock);
	list_add(&dentry->d_alias, &inode->i_dentry);
	spinlock_release(&inode->i_lock);

	spinlock_acquire(&dentry->d_lock);
	dentry->d_flags &= ~DCACHE_NEGATIVE;
	spinlock_release(&dentry->d_lock);
}

/* Increment the reference count of @dentry */
struct dentry *dentry_dup(struct dentry *dentry)
{
	log_trace("%s(): Duplicating dentry: name=%.*s, inode=%ld, refcnt=%d\n",
		  __func__, dentry->d_name.len, dentry->d_name.name,
		  dentry->d_inode->i_ino, arc_get(&dentry->d_count));
	arc_inc(&dentry->d_count);
	return dentry;
}

/* Decrement the reference count of @dentry */
void dentry_put(struct dentry *dentry)
{
	const struct dentry_operations *op;
	struct inode *inode;

	log_trace("%s(): Putting dentry: name=%.*s, inode=%ld, refcnt=%d\n",
		  __func__, dentry->d_name.len, dentry->d_name.name,
		  dentry->d_inode->i_ino, arc_get(&dentry->d_count));

	if (arc_dec_fetch(&dentry->d_count) > 0)
		return;

	log_trace("%s(): Freeing dentry: name=%.*s, inode=%ld\n", __func__,
		  dentry->d_name.len, dentry->d_name.name,
		  dentry->d_inode->i_ino);

	if (dentry->d_parent != dentry) {
		spinlock_acquire(&dentry->d_parent->d_lock);
		list_del(&dentry->d_child);
		spinlock_release(&dentry->d_parent->d_lock);
		dentry_put(dentry->d_parent);
	}

	if (!(dentry->d_flags & DCACHE_UNHASHED)) {
		log_trace(
			"%s(): Removing dentry from hash table: name=%.*s, inode=%ld\n",
			__func__, dentry->d_name.len, dentry->d_name.name,
			dentry->d_inode->i_ino);
		spinlock_acquire(&dentry_htable_lock);
		assert(!hlist_unhashed(&dentry->d_hash));
		hlist_del_init(&dentry->d_hash);
		spinlock_release(&dentry_htable_lock);
	}

	if (!(dentry->d_flags & DCACHE_NEGATIVE)) {
		inode = dentry->d_inode;

		spinlock_acquire(&inode->i_lock);
		list_del(&dentry->d_alias);
		spinlock_release(&inode->i_lock);

		op = dentry->d_op;
		if (op->release)
			op->release(dentry);

		if (op->iput)
			op->iput(dentry, inode);
		else
			inode_put(inode);
	}

	__free_dentry(dentry);
}

static struct dentry *dentry_lookup_locked(struct hlist_head *head,
					   struct dentry *parent,
					   const struct qstr *name)
{
	struct dentry *dentry;

	assert(spinlock_holding(&dentry_htable_lock));

	hlist_for_each_entry(dentry, head, d_hash) {
		if (dentry->d_parent == parent &&
		    dentry->d_name.hash == name->hash &&
		    dentry->d_name.len == name->len &&
		    !dentry->d_op->compare(dentry, name->len, name->name,
					   &dentry->d_name)) {
			arc_inc(&dentry->d_count);
			return dentry;
		}
	}

	return NULL;
}

/**
 * dentry_lookup() - Resolve one path component under @parent
 * @parent: parent dentry
 * @name: component name with hash prepared by caller
 *
 * Lookup first checks the dcache hash. On miss, it allocates a child dentry and
 * invokes parent inode ->lookup(). The returned dentry may be positive (existing)
 * or negative (DCACHE_NEGATIVE set, indicating not found).
 *
 * Return: The dentry on success or ERR_PTR(-errno) on failure.
 */
struct dentry *dentry_lookup(struct dentry *parent, const struct qstr *name)
{
	struct inode *inode;
	struct dentry *dentry, *tmp;
	struct hlist_head *head;

	uint32_t idx = name->hash & (DENTRY_HTABLE_SIZE - 1);
	head = &dentry_htable[idx];

	log_trace("%s(): Looking up %.*s in %.*s\n", __func__, name->len,
		  name->name, parent->d_name.len, parent->d_name.name);

	spinlock_acquire(&dentry_htable_lock);
	dentry = dentry_lookup_locked(head, parent, name);
	spinlock_release(&dentry_htable_lock);
	if (dentry) {
		log_trace("%s(): Cache hit for %.*s\n", __func__, name->len,
			  name->name);
		return dentry;
	}
	log_trace("%s(): Cache miss for %.*s\n", __func__, name->len,
		  name->name);

	dentry = alloc_dentry(parent, name);
	if (!dentry)
		return ERR_PTR(-ENOMEM);

	inode = parent->d_inode;
	sleeplock_acquire(&inode->i_rwsem);
	tmp = inode->i_op->lookup(inode, dentry, 0);
	sleeplock_release(&inode->i_rwsem);
	if (IS_ERR(tmp)) {
		dentry->d_flags |= DCACHE_NEGATIVE;
		dentry_put(dentry);
		return ERR_CAST(tmp);
	}

	if (!tmp)
		dentry->d_flags |= DCACHE_NEGATIVE;
	else
		dentry = tmp;

	spinlock_acquire(&dentry->d_lock);
	if (dentry->d_parent != dentry && dentry->d_flags & DCACHE_UNHASHED) {
		dentry->d_flags &= ~DCACHE_UNHASHED;
		spinlock_acquire(&dentry_htable_lock);
		hlist_add_head(&dentry->d_hash, &dentry_htable[idx]);
		spinlock_release(&dentry_htable_lock);
		spinlock_release(&dentry->d_lock);
		log_trace(
			"%s(): Added dentry to hash table: name=%.*s, inode=%ld\n",
			__func__, dentry->d_name.len, dentry->d_name.name,
			dentry->d_inode->i_ino);
	} else {
		spinlock_release(&dentry->d_lock);
		log_trace(
			"%s(): Dentry already in hash table: name=%.*s, inode=%ld\n",
			__func__, dentry->d_name.len, dentry->d_name.name,
			dentry->d_inode->i_ino);
	}

	return dentry;
}

/**
 * dentry_splice_alias() - Attach @inode to @dentry or reuse existing alias
 * @inode: inode to splice the dentry into
 * @dentry: dentry to splice
 *
 * Return: The alias dentry on success or ERR_PTR(-errno) on failure.
 */
struct dentry *dentry_splice_alias(struct inode *inode, struct dentry *dentry)
{
	struct list_head *dentries;
	struct dentry *d;

	if (!inode)
		return ERR_PTR(-EINVAL);

	dentries = &inode->i_dentry;

	spinlock_acquire(&inode->i_lock);
	list_for_each_entry(d, dentries, d_alias) {
		if (d->d_parent == dentry->d_parent &&
		    d->d_name.hash == dentry->d_name.hash &&
		    d->d_name.len == dentry->d_name.len &&
		    !d->d_op->compare(d, dentry->d_name.len,
				      dentry->d_name.name, &d->d_name)) {
			spinlock_release(&inode->i_lock);
			dentry_dup(d);
			dentry->d_flags |= DCACHE_NEGATIVE;
			dentry_put(dentry);
			return d;
		}
	}
	list_add(&dentry->d_alias, dentries);
	dentry->d_inode = inode;
	spinlock_release(&inode->i_lock);

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

static int generic_dop_compare(const struct dentry *dentry, unsigned int len,
			       const char *str, const struct qstr *name)
{
	if (len > name->len)
		return 1;
	if (len < name->len)
		return -1;
	return memcmp(str, name->name, len);
}

const struct dentry_operations generic_dop = {
	.compare = generic_dop_compare,
};

void dcache_dump(void)
{
	struct dentry *d;
	struct hlist_head *head;

	spinlock_acquire(&dentry_htable_lock);
	printk("Dcache dump:\n");
	for (size_t i = 0; i < DENTRY_HTABLE_SIZE; i++) {
		head = &dentry_htable[i];
		if (hlist_empty(head))
			continue;
		printk("Bucket %zu:\n", i);
		hlist_for_each_entry(d, head, d_hash) {
			printk("  Name: %s, Inode: %ld, Refcnt: %d, Negative: %s, Mounted: %s\n",
			       d->d_name.name, d->d_inode->i_ino,
			       arc_get(&d->d_count),
			       d->d_flags & DCACHE_NEGATIVE ? "true" : "false",
			       d->d_flags & DCACHE_MOUNTED ? "true" : "false");
		}
	}
	spinlock_release(&dentry_htable_lock);
}
