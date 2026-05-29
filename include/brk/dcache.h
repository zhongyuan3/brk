#ifndef BRK_DCACHE_H
#define BRK_DCACHE_H

#include <brk/bits.h>
#include <brk/hash.h>
#include <brk/refcnt_types.h>
#include <brk/spinlock_types.h>
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
	unsigned int flags;
	spinlock_t lock;
	struct fs_inode *inode;
	struct list_head alias;
	struct fs_dentry *parent;
	struct list_head child;
	struct list_head children;
	struct hlist_node hash;
	struct qstr name;
	union {
		char short_name[DENTRY_SHORT_NAME_SIZE];
		char *long_name;
	};
	struct fs_super_block *sb;
	const struct fs_dentry_ops *ops;
	void *private_data;
};

struct fs_dentry_ops {
	int (*compare)(const struct fs_dentry *, unsigned int, const char *,
		       const struct qstr *);
	void (*release)(struct fs_dentry *);
	void (*iput)(struct fs_dentry *, struct fs_inode *);
};

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
