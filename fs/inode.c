#include <aosd/align.h>
#include <aosd/assert.h>
#include <aosd/dcache.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/panic.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/types.h>

static list_head_define(ilist);
static spinlock_define(ilist_lock);
static struct kmem_cache icache;

int inode_cache_init(void)
{
	return kmem_cache_init(&icache, sizeof(struct inode),
			       alignof(struct inode), "icache");
}

static struct inode *__inode_alloc(void)
{
	struct inode *ip;

	ip = kmem_cache_alloc(&icache);
	if (ip)
		memset(ip, 0, sizeof(*ip));
	return ip;
}

static void __inode_free(struct inode *ip)
{
	kmem_cache_free(&icache, ip);
}

static void __inode_add(struct inode *ip)
{
	list_add(&ip->i_list, &ilist);
}

static void __inode_del(struct inode *ip)
{
	list_del(&ip->i_list);
}

struct inode *inode_alloc(void)
{
	struct inode *ip;

	ip = __inode_alloc();
	if (!ip)
		return NULL;
	ip->i_rc = 1;
	list_init_head(&ip->i_list);
	sleeplock_init(&ip->i_lock, "inode");
	return ip;
}

void inode_free(struct inode *ip)
{
	__inode_free(ip);
}

int inode_add(struct inode *ip)
{
	spinlock_acquire(&ilist_lock);
	__inode_add(ip);
	spinlock_release(&ilist_lock);
	return 0;
}

struct inode *inode_dup(struct inode *ip)
{
	assert(ip->i_rc > 0);
	spinlock_acquire(&ilist_lock);
	++ip->i_rc;
	spinlock_release(&ilist_lock);
	return ip;
}

void inode_put(struct inode *ip)
{
	assert(ip->i_rc > 0);
	spinlock_acquire(&ilist_lock);
	--ip->i_rc;
	if (ip->i_rc == 0) {
		__inode_del(ip);
		spinlock_release(&ilist_lock);
		ip->i_sb->s_ops->deinit_inode(ip);
		sblock_put(ip->i_sb);
		inode_free(ip);
	} else {
		spinlock_release(&ilist_lock);
	}
}

mode_t inode_mode(struct inode *ip)
{
	mode_t mode;
	sleeplock_acquire(&ip->i_lock);
	mode = ip->i_mode;
	sleeplock_release(&ip->i_lock);
	return mode;
}

int inode_rc(struct inode *ip)
{
	int rc;
	spinlock_acquire(&ilist_lock);
	rc = ip->i_rc;
	spinlock_release(&ilist_lock);
	return rc;
}
