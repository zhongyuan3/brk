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

static struct list_head itable[NR_ITABLE_BUCKETS];
static SPINLOCK_DEFINE(itable_lock);
static struct kmem_cache icache;

int inode_cache_init(void)
{
	for (int i = 0; i < NR_ITABLE_BUCKETS; ++i)
		list_init(&itable[i]);
	return kmem_cache_init(&icache, sizeof(struct inode),
			       alignof(struct inode), "icache");
}

static inline uint32_t inode_hash(struct super_block *sb, uint32_t ino)
{
	const uint8_t *k = (uint8_t *)&sb;
	uint32_t h = 0x811c9dc5;
	for (size_t i = 0; i < sizeof(void *); ++i) {
		h ^= k[i];
		h *= 0x01000193;
	}
	k = (uint8_t *)&ino;
	for (size_t i = 0; i < sizeof(ino); ++i) {
		h ^= k[i];
		h *= 0x01000193;
	}
	return h;
}

static struct inode *__inode_get(struct super_block *sb, uint32_t ino)
{
	struct inode *ip;
	uint32_t idx = inode_hash(sb, ino) % NR_ITABLE_BUCKETS;
	struct list_head *bkt = &itable[idx];

	list_for_each_entry(ip, bkt, i_list)
		if (ip->i_no == ino && ip->i_sb == sb) {
			++ip->i_rc;
			return ip;
		}

	return NULL;
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
	uint32_t idx = inode_hash(ip->i_sb, ip->i_no) % NR_ITABLE_BUCKETS;
	list_add(&ip->i_list, &itable[idx]);
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
	list_init(&ip->i_list);
	sleeplock_init(&ip->i_lock, "inode");
	return ip;
}

void inode_free(struct inode *ip)
{
	__inode_free(ip);
}

int inode_add(struct inode *ip)
{
	spinlock_acquire(&itable_lock);
	__inode_add(ip);
	spinlock_release(&itable_lock);
	return 0;
}

struct inode *inode_dup(struct inode *ip)
{
	assert(ip->i_rc > 0);
	spinlock_acquire(&itable_lock);
	++ip->i_rc;
	spinlock_release(&itable_lock);
	return ip;
}

void inode_put(struct inode *ip)
{
	assert(ip->i_rc > 0);
	spinlock_acquire(&itable_lock);
	--ip->i_rc;
	if (ip->i_rc == 0) {
		__inode_del(ip);
		spinlock_release(&itable_lock);
		ip->i_sb->s_ops->deinit_inode(ip);
		sblock_put(ip->i_sb);
		inode_free(ip);
	} else {
		spinlock_release(&itable_lock);
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

refcnt_t inode_rc(struct inode *ip)
{
	refcnt_t rc;
	spinlock_acquire(&itable_lock);
	rc = ip->i_rc;
	spinlock_release(&itable_lock);
	return rc;
}

struct inode *inode_get(struct super_block *sb, uint32_t ino)
{
	struct inode *ip;

	spinlock_acquire(&itable_lock);
	ip = __inode_get(sb, ino);
	spinlock_release(&itable_lock);
	return ip;
}
