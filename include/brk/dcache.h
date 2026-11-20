#ifndef BRK_DCACHE_H
#define BRK_DCACHE_H

#include <brk/bits.h>
#include <brk/hash.h>
#include <brk/refcnt_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <stdbool.h>

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

struct dentry {
	refcnt_t count;
	unsigned int flags;
	spinlock_t lock;
	struct inode *inode;
	struct list_head alias;
	struct dentry *parent;
	struct list_head child;
	struct list_head children;
	struct hlist_node hash;
	struct qstr name;
	union {
		char short_name[DENTRY_SHORT_NAME_SIZE];
		char *long_name;
	};
	struct super_block *sb;
	const struct dentry_ops *ops;
	void *private_data;
};

struct dentry_ops {
	int (*compare)(const struct dentry *, unsigned int, const char *,
		       const struct qstr *);
	void (*release)(struct dentry *);
	void (*iput)(struct dentry *, struct inode *);
};

struct dentry *dentry_make_root(struct inode *root_inode);
struct dentry *dentry_alloc_anon(struct inode *inode, const struct qstr *name);
void dentry_instantiate(struct dentry *dentry, struct inode *inode);
void dentry_make_negative(struct dentry *dentry);
struct dentry *dentry_get(struct dentry *dentry);
void dentry_put(struct dentry *dentry);
/*
 * Locked accessors for the dentry state that path walking reads without
 * holding any other lock.  Use these instead of reading ->flags / ->inode
 * directly.
 */
bool dentry_is_negative(struct dentry *dentry);
bool dentry_is_mounted(struct dentry *dentry);
struct inode *dentry_inode(struct dentry *dentry);
struct dentry *dentry_lookup(struct dentry *parent, const struct qstr *name);
struct dentry *dentry_splice_alias(struct inode *inode, struct dentry *dentry);
void dentry_cache_init(void);

extern const struct qstr slash_name;
extern const struct qstr dot_name;
extern const struct qstr dotdot_name;

#define SLASH_NAME_HASH 0x2a0c975e
#define DOT_NAME_HASH 0x2b0c98f1
#define DOTDOT_NAME_HASH 0xa3d4a70d

extern const struct dentry_ops generic_dop;

#endif
