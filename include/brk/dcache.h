#ifndef BRK_DCACHE_H
#define BRK_DCACHE_H

#include <brk/hash.h>
#include <brk/lock.h>
#include <brk/macros.h>
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
	uint32_t hash;
	uint32_t len;
};

#define QSTR_MAKE(n, l)                                     \
	(struct qstr)                                       \
	{                                                   \
		.name = n, .hash = fnv1a_32(n, l), .len = l \
	}

struct dentry {
	arc_t d_count;

	unsigned int d_flags; /* protected by d_lock */
	spinlock_t d_lock;

	/* Positive dentry: points to inode; negative dentry: NULL. */
	struct inode *d_inode;
	struct list_head d_alias; /* protected by d_inode->i_lock */

	struct dentry *d_parent; /* hold reference count, self if root (no ref) */
	struct list_head d_child; /* protected by d_parent->d_lock */
	struct list_head d_subdirs; /* protected by d_lock */

	struct hlist_node d_hash; /* protected by dentry_htable_lock */

	struct qstr d_name;
	union {
		char d_short_name[DENTRY_SHORT_NAME_SIZE];
		char *d_long_name;
	};

	struct super_block *d_sb; /* no reference count */

	/*
	 * Dentry operation table.
	 * Recommended invariant: newly allocated dentries always have a valid
	 * d_op (at least &generic_dop) before they are visible in lookup paths.
	 */
	const struct dentry_operations *d_op;

	void *d_fsdata;
};

struct dentry_operations {
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
	int (*compare)(const struct dentry *dentry, unsigned int len,
		       const char *str, const struct qstr *name);

	/**
	 * release() - dentry memory will be freed
	 * @dentry: dentry being destroyed
	 *
	 * Release @d_fsdata and similar.
	 */
	void (*release)(struct dentry *dentry);

	/**
	 * iput() - dentry is dropping its inode reference
	 * @dentry: the dentry that is dropping its inode reference
	 * @inode: the inode that is about to be inode_put()
	 *
	 * If %NULL, the VFS calls inode_put() directly.
	 */
	void (*iput)(struct dentry *dentry, struct inode *inode);
};

/*
 * Dcache locking guideline:
 *   dentry_htable_lock -> dentry.d_lock -> inode.i_lock
 *
 * Notes:
 * - dentry lookup should be lockless outside the hash critical section as much
 *   as possible; do not call filesystem lookup callbacks while holding
 *   dentry_htable_lock.
 * - Parent/child list updates should hold parent->d_lock.
 * - Alias list updates should hold inode->i_lock.
 */

struct dentry *dentry_make_root(struct inode *root_inode);
struct dentry *dentry_alloc_anon(struct inode *inode, const struct qstr *name);
void dentry_instantiate(struct dentry *dentry, struct inode *inode);
struct dentry *dentry_dup(struct dentry *dentry);
void dentry_put(struct dentry *dentry);
struct dentry *dentry_lookup(struct dentry *parent, const struct qstr *name);
struct dentry *dentry_splice_alias(struct inode *inode, struct dentry *dentry);
void dentry_cache_init(void);

extern const struct qstr slash_name;
extern const struct qstr dot_name;
extern const struct qstr dotdot_name;

#define SLASH_NAME_HASH 0x2a0c975e
#define DOT_NAME_HASH 0x2b0c98f1
#define DOTDOT_NAME_HASH 0xa3d4a70d

extern const struct dentry_operations generic_dop;

void dcache_dump(void);

#endif
