#ifndef AOSD_DCACHE_H
#define AOSD_DCACHE_H

#include <aosd/types.h>

#define NR_DTABLE_BUCKETS 64

#define DENTRY_MOUNTED (1 << 0)

struct inode;
struct dentry;
struct dentry_operations;

struct dentry {
	const char *d_name;
	int d_rc;
	unsigned int d_flags;
	struct list_head d_hash;
	struct dentry *d_parent;
	struct inode *d_inode;
	struct dentry_operations *d_ops;
};

struct dentry_operations {
	int (*compare)(struct dentry *, const char *, size_t);
};

int dentry_cache_init(void);
struct dentry *dentry_get(struct dentry *parent, const char *name);
struct dentry *dentry_dup(struct dentry *dp);
void dentry_put(struct dentry *dp);
struct dentry *dentry_alloc(const char *name, size_t len);
void dentry_free(struct dentry *dp);
int dentry_add(struct dentry *dp);
int dentry_rc(struct dentry *dp);
unsigned int dentry_flags(struct dentry *dp);

#endif
