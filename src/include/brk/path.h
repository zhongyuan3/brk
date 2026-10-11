#ifndef BRK_PATH_H
#define BRK_PATH_H

#include <brk/bits.h>
#include <brk/dcache.h>
#include <brk/fs_types.h>
#include <brk/types.h>

/* pathwalk mode */
#define LOOKUP_FOLLOW BIT(0) /* follow links at the end */
#define LOOKUP_DIRECTORY BIT(1) /* require a directory */
#define LOOKUP_AUTOMOUNT BIT(2) /* force terminal automount */
#define LOOKUP_EMPTY BIT(3) /* accept empty path [user_... only] */
#define LOOKUP_LINKAT_EMPTY BIT(4) /* Linkat request with empty path. */
#define LOOKUP_DOWN BIT(5) /* follow mounts in the starting point */
#define LOOKUP_MOUNTPOINT BIT(6) /* follow mounts in the end */
#define LOOKUP_REVAL BIT(7) /* tell ->d_revalidate() to trust no cache */
#define LOOKUP_RCU BIT(8) /* RCU pathwalk mode; semi-internal */
#define LOOKUP_CACHED BIT(9) /* Only do cached lookup */
#define LOOKUP_PARENT BIT(10) /* Looking up final parent in path */
/* 5 spare bits for pathwalk */

/* These tell filesystem methods that we are dealing with the final component... */
#define LOOKUP_OPEN BIT(16) /* ... in open */
#define LOOKUP_CREATE BIT(17) /* ... in object creation */
#define LOOKUP_EXCL BIT(18) /* ... in target must not exist */
#define LOOKUP_RENAME_TARGET BIT(19) /* ... in destination of rename() */

/* 4 spare bits for intent */

/* Scoping flags for lookup. */
#define LOOKUP_NO_SYMLINKS BIT(24) /* No symlink crossing. */
#define LOOKUP_NO_MAGICLINKS BIT(25) /* No nd_jump_link() crossing. */
#define LOOKUP_NO_XDEV BIT(26) /* No mountpoint crossing. */
#define LOOKUP_BENEATH BIT(27) /* No escaping from starting point. */
#define LOOKUP_IN_ROOT BIT(28) /* Treat dirfd as fs root. */
/* LOOKUP_* flags which do scope-related checks based on the dirfd. */
#define LOOKUP_IS_SCOPED (LOOKUP_BENEATH | LOOKUP_IN_ROOT)
/* 3 spare bits for scoping */

struct fs_path {
	struct fs_mount_state *mnt;
	struct fs_dentry *dentry;
};

int fs_path_lookup_at(int dir_fd, const char *name, unsigned int flags,
		      struct fs_path *path);
int fs_path_lookup(const char *name, unsigned int flags, struct fs_path *path);
int fs_path_parent_at(int dir_fd, const char *name, struct fs_path *path,
		      struct qstr *last_component);
void fs_path_get(struct fs_path *path);
void fs_path_put(struct fs_path *path);
int fs_path_to_absolute(const struct fs_path *path, char *buf, size_t bufsz);
int fs_path_dot(struct fs_path *path, struct fs_path *dot);
int fs_path_dot_dot(struct fs_path *path, struct fs_path *dotdot);

#endif
