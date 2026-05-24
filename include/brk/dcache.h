#ifndef BRK_DCACHE_H
#define BRK_DCACHE_H

#include <brk/bits.h>
#include <brk/hash.h>
#include <brk/lock.h>
#include <brk/refcnt.h>
#include <brk/types.h>

#define DENTRY_HTABLE_BITS 8
#define DENTRY_HTABLE_SIZE (1 << DENTRY_HTABLE_BITS)

#define DCACHE_NEGATIVE BIT(0)
#define DCACHE_MOUNTED BIT(1)
#define DCACHE_UNHASHED BIT(2)

#define DENTRY_SHORT_NAME_SIZE 32

struct qstr {
	const char *name;
	u32 hash;
	u32 len;
};

#define QSTR_MAKE(n, l)                                     \
	(struct qstr)                                       \
	{                                                   \
		.name = n, .hash = fnv1a_32(n, l), .len = l \
	}

struct fs_dentry {
	refcnt_t count;

	unsigned int flags; /* protected by d_lock */
	spinlock_t lock;

	/* Positive dentry: points to inode; negative dentry: NULL. */
	struct fs_inode *inode;
	struct list_head alias; /* protected by d_inode->lock */

	struct fs_dentry
		*parent; /* hold reference count, self if root (no ref) */
	struct list_head child; /* protected by d_parent->d_lock */
	struct list_head children; /* protected by d_lock */

	struct hlist_node hash; /* protected by dentry_htable_lock */

	struct qstr name;
	union {
		char short_name[DENTRY_SHORT_NAME_SIZE];
		char *long_name;
	};

	struct fs_super_block *sb; /* no reference count */

	/*
	 * Dentry operation table.
	 * Recommended invariant: newly allocated dentries always have a valid
	 * d_op (at least &generic_dop) before they are visible in lookup paths.
	 */
	const struct fs_dentry_ops *ops;

	void *private_data;
};

struct fs_dentry_ops {
	/**
	 * compare() - compare a candidate name with a dentry name
	 * @dentry: cached candidate dentry being tested
	 * @len: length of the name to compare
	 * @str: name bytes to compare
	 * @name: full struct qstr for the cached name
	 *
	 * If %NULL, byte comparison is used. Case-insensitive filesystems
	 * override this (e.g. FAT32).
	 *
	 * Return: %0 if equal, non-zero if different.
	 */
	int (*compare)(const struct fs_dentry *dentry, unsigned int len,
		       const char *str, const struct qstr *name);

	/**
	 * release() - dentry memory will be freed
	 * @dentry: dentry being destroyed
	 *
	 * Release @private_data and similar.
	 */
	void (*release)(struct fs_dentry *dentry);

	/**
	 * iput() - dentry is dropping its inode reference
	 * @dentry: the dentry that is dropping its inode reference
	 * @inode: the inode that is about to be fs_inode_put()
	 *
	 * If %NULL, the VFS calls fs_inode_put() directly.
	 */
	void (*iput)(struct fs_dentry *dentry, struct fs_inode *inode);
};

/*
 * Dcache locking guideline:
 *   dentry_htable_lock -> dentry.d_lock -> inode.lock
 *
 * Notes:
 * - dentry lookup should be lockless outside the hash critical section as much
 *   as possible; do not call filesystem lookup callbacks while holding
 *   dentry_htable_lock.
 * - Parent/child list updates should hold parent->d_lock.
 * - Alias list updates should hold inode->lock.
 */

struct fs_dentry *fs_dentry_make_root(struct fs_inode *root_inode);
struct fs_dentry *fs_dentry_alloc_anon(struct fs_inode *inode,
				       const struct qstr *name);
void fs_dentry_instantiate(struct fs_dentry *dentry, struct fs_inode *inode);
struct fs_dentry *fs_dentry_get(struct fs_dentry *dentry);
void fs_dentry_put(struct fs_dentry *dentry);
struct fs_dentry *fs_dentry_lookup(struct fs_dentry *parent,
				   const struct qstr *name);
struct fs_dentry *fs_dentry_splice_alias(struct fs_inode *inode,
					 struct fs_dentry *dentry);
void fs_dentry_cache_init(void);

extern const struct qstr slash_name;
extern const struct qstr dot_name;
extern const struct qstr dotdot_name;

#define SLASH_NAME_HASH 0x2a0c975e
#define DOT_NAME_HASH 0x2b0c98f1
#define DOTDOT_NAME_HASH 0xa3d4a70d

extern const struct fs_dentry_ops generic_dop;

void fs_dcache_dump(void);

#endif
