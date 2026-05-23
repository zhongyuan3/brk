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

/*
 * Path-walk contract and design notes
 * -----------------------------------
 * 1) A resolved location is always (mnt, dentry) together. Either field alone
 *    is insufficient in the presence of mount namespaces / stacked mounts.
 *
 * 2) Reference ownership:
 *      - successful path_lookup/path_lookupat returns with one ref held on
 *        both path->mnt and path->dentry.
 *      - callers must eventually call path_put() exactly once.
 *      - path_get()/path_put() are strict pair operations.
 *
 * 3) Name component handling (recommended semantics for implementation):
 *      - absolute paths start from process root.
 *      - relative paths start from dir_fd (or cwd when dir_fd is AT_FDCWD).
 *      - "." keeps current path unchanged.
 *      - ".." climbs to parent; when at a mount root, crossing to the parent
 *        mount should be handled before climbing dentries.
 *      - when a dentry is marked mounted, walk should follow into child mount
 *        root (subject to lookup flags/policy).
 *
 * 4) LOOKUP_* flags:
 *      - LOOKUP_FOLLOW: follow terminal symlink if implementation supports it.
 *      - LOOKUP_PARENT: resolve parent and leave last component to caller
 *        (e.g., create/unlink/rename style operations).
 *
 * 5) Error-return convention for lookup helpers:
 *      - return 0 on success and fill @path
 *      - return negative errno on failure
 *      - on failure, @path must not hold a newly acquired live reference.
 *
 * 6) Locking guidance:
 *      - path walk should avoid holding global hash spinlocks across
 *        filesystem callbacks (lookup/permission/getattr).
 *      - keep critical sections short; prefer per-object locks (dentry/inode/
 *        mount) with consistent lock ordering documented in fs.h/mount.h/
 *        dcache.h.
 */

/**
 * struct path - Describes a path point in the file system
 * @mnt: Mount point
 * @dentry: Directory entry
 *
 * A path can only be uniquely determined by holding both a mount point and a directory entry.
 * Each path holds references to both mnt and dentry (increments their respective reference counts).
 */
struct path {
	struct mount_instance *mnt;
	struct dentry *dentry;
};

/**
 * path_lookupat() - Resolve a path from a dirfd context
 * @dir_fd: directory fd or AT_FDCWD-style special value
 * @name: input path string
 * @flags: LOOKUP_* flags
 * @path: output resolved path (refcounted on success)
 *
 * Return: 0 on success, negative errno on failure.
 */
int path_lookupat(int dir_fd, const char *name, unsigned int flags,
		  struct path *path);
/**
 * path_lookup() - Resolve a path from current process context
 * @name: input path string
 * @flags: LOOKUP_* flags
 * @path: output resolved path (refcounted on success)
 *
 * Return: 0 on success, negative errno on failure.
 */
int path_lookup(const char *name, unsigned int flags, struct path *path);
/**
 * path_parentat() - Resolve a parent path from a dirfd context
 * @dir_fd: directory fd or AT_FDCWD-style special value
 * @name: input path string
 * @path: output resolved path (refcounted on success)
 * @last_component: output last component (refcounted on success)
 *
 * Return: 0 on success, negative errno on failure.
 */
int path_parentat(int dir_fd, const char *name, struct path *path,
		  struct qstr *last_component);
/* Increment refs for both @path->mnt and @path->dentry. */
void path_get(struct path *path);
/* Drop refs for both @path->mnt and @path->dentry. */
void path_put(struct path *path);
/*
 * Convert a resolved path object to absolute string form.
 * Returns 0 on success, negative errno on failure.
 */
int path_to_absolute(const struct path *path, char *buf, usize_t bufsz);

int path_dot(struct path *path, struct path *dot);
int path_dot_dot(struct path *path, struct path *dotdot);

#endif
