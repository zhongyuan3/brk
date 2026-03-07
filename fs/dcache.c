#include <aosd/align.h>
#include <aosd/assert.h>
#include <aosd/dcache.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/mount.h>
#include <aosd/printk.h>
#include <aosd/sched.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/types.h>

static struct list_head dtable[NR_DTABLE_BUCKETS];
static spinlock_define(dtable_lock);
static struct kmem_cache dcache;

int dentry_cache_init(void)
{
	for (int i = 0; i < NR_DTABLE_BUCKETS; ++i)
		list_init_head(&dtable[i]);
	return kmem_cache_init(&dcache, sizeof(struct dentry),
			       alignof(struct dentry), "dcache");
}

static struct dentry *__dentry_alloc(void)
{
	struct dentry *dp;

	dp = kmem_cache_alloc(&dcache);
	if (dp)
		memset(dp, 0, sizeof(*dp));
	return dp;
}

static void __dentry_free(struct dentry *dp)
{
	kmem_cache_free(&dcache, dp);
}

static uint32_t dentry_hash(struct dentry *parent, const char *name)
{
	uint32_t h = 0;
	uint64_t a = (uint64_t)parent;

	h = (h << 5) - h + (a & 0xFFFFFFFF);
	h = (h << 5) - h + (a >> 32);

	if (name)
		while (*name)
			h = (h << 5) - h + *name++;

	h = (h << 5) - h + (h >> 16);

	return h % countof(dtable);
}

static void __dentry_add(struct dentry *dp)
{
	struct list_head *bkt;

	bkt = dtable + dentry_hash(dp->d_parent, dp->d_name);
	list_add(&dp->d_hash, bkt);
}

static void __dentry_del(struct dentry *dp)
{
	list_del(&dp->d_hash);
}

static struct dentry *__dentry_get(struct dentry *parent, const char *name)
{
	struct dentry *dp = NULL;
	size_t len = strlen(name);
	struct list_head *bkt = dtable + dentry_hash(parent, name);

	if (list_empty(bkt))
		return NULL;

	list_for_each_entry(dp, bkt, d_hash) {
		if (dp->d_parent == parent &&
		    !dp->d_ops->compare(dp, name, len)) {
			++dp->d_rc;
			return dp;
		}
	}

	return NULL;
}

static void dentry_destroy(struct dentry *dp)
{
	assert(dp);
	assert(dp->d_rc == 0);
	if (dp->d_parent)
		dentry_put(dp->d_parent);
	dp->d_inode->i_dentry = NULL;
	inode_put(dp->d_inode);
	dentry_free(dp);
}

struct dentry *dentry_alloc(const char *name, size_t len)
{
	char *dname;
	struct dentry *dp;

	dp = __dentry_alloc();
	if (!dp)
		return NULL;

	dname = kmalloc(len + 1);
	if (!dname) {
		__dentry_free(dp);
		return NULL;
	}

	memcpy(dname, name, len);
	dname[len] = '\0';
	dp->d_rc = 1;
	dp->d_name = dname;
	list_init_head(&dp->d_hash);
	return dp;
}

void dentry_free(struct dentry *dp)
{
	assert(dp);
	kfree((void *)dp->d_name);
	__dentry_free(dp);
}

int dentry_add(struct dentry *dp)
{
	assert(dp);
	spinlock_acquire(&dtable_lock);
	__dentry_add(dp);
	spinlock_release(&dtable_lock);
	return 0;
}

struct dentry *dentry_get(struct dentry *parent, const char *name)
{
	struct dentry *dp;
	struct dentry *newdp;

	spinlock_acquire(&dtable_lock);
	dp = __dentry_get(parent, name);
	if (dp) {
		spinlock_release(&dtable_lock);
		return dp;
	}
	spinlock_release(&dtable_lock);

	if (!parent)
		return NULL;

	newdp = dentry_alloc(name, strlen(name));
	if (!newdp)
		return NULL;
	if (parent->d_inode->i_ops->lookup(parent->d_inode, newdp) != 0) {
		dentry_free(newdp);
		return NULL;
	}
	newdp->d_parent = dentry_dup(parent);

	spinlock_acquire(&dtable_lock);
	dp = __dentry_get(parent, name);
	if (dp) {
		spinlock_release(&dtable_lock);
		dentry_destroy(newdp);
		return dp;
	}
	__dentry_add(newdp);
	spinlock_release(&dtable_lock);

	return newdp;
}

void dentry_put(struct dentry *dp)
{
	assert(dp);
	assert(dp->d_rc > 0);
	spinlock_acquire(&dtable_lock);
	--dp->d_rc;
	if (dp->d_rc <= 0) {
		__dentry_del(dp);
		spinlock_release(&dtable_lock);
		dentry_destroy(dp);
	} else {
		spinlock_release(&dtable_lock);
	}
}

struct dentry *dentry_dup(struct dentry *dp)
{
	assert(dp);
	assert(dp->d_rc > 0);
	spinlock_acquire(&dtable_lock);
	++dp->d_rc;
	spinlock_release(&dtable_lock);
	return dp;
}

int dentry_rc(struct dentry *dp)
{
	assert(dp);
	int rc;
	spinlock_acquire(&dtable_lock);
	rc = dp->d_rc;
	spinlock_release(&dtable_lock);
	return rc;
}

unsigned int dentry_flags(struct dentry *dp)
{
	assert(dp);
	unsigned int flags;
	spinlock_acquire(&dtable_lock);
	flags = dp->d_flags;
	spinlock_release(&dtable_lock);
	return flags;
}
