#ifndef AOSD_PATH_H
#define AOSD_PATH_H

#include <aosd/types.h>

struct dentry;

char *path_get_full(struct dentry *dp);
int path_get_last(const char *path, const char **pstart, size_t *plen);
int path_cat(char *path, const char *src, size_t len);
struct dentry *path_lookup_at(struct dentry *dir, const char *path);
struct dentry *path_lookup_parent_at(struct dentry *dir, const char *path,
				     char *name, size_t size);
struct dentry *path_lookup(const char *path);
struct dentry *path_lookup_parent(const char *path, char *name, size_t size);

#endif
