#ifndef BRK_FSINFO_H
#define BRK_FSINFO_H

#include <brk/base/types.h>
#include <brk/fs/fs_types.h>
#include <brk/fs/path.h>
#include <brk/lib/refcnt_types.h>
#include <brk/lock/spinlock_types.h>

struct file_system_info {
	struct fs_path cwd, root;
	spinlock_t lock;
	refcnt_t refcnt;
};

struct file_system_info *fsinfo_alloc(void);
void fsinfo_put(struct file_system_info *info);
struct file_system_info *fsinfo_get(struct file_system_info *info);

/**
 * fsinfo_copy() - Copy a file system info
 * @dst: The file system info to copy to
 * @src: The file system info to copy from
 *
 * @dst must be empty before calling this function.
 */
void fsinfo_copy(struct file_system_info *dst, struct file_system_info *src);

/**
 * fsinfo_set_cwd() - Set the current working directory of the file system info
 * @info: The file system info to set the current working directory for
 * @path: The new current working directory
 *
 * This function takes ownership of the @path.
 */
void fsinfo_set_cwd(struct file_system_info *info, struct fs_path *path);

/**
 * fsinfo_set_root() - Set the root directory of the file system info
 * @info: The file system info to set the root directory for
 * @path: The new root directory
 *
 * This function takes ownership of the @path.
 */
void fsinfo_set_root(struct file_system_info *info, struct fs_path *path);

/**
 * fsinfo_update_cwd() - Update the current working directory of the file system info
 * @info: The file system info to update the current working directory for
 * @path: The new current working directory
 *
 * Puts the old current working directory and takes ownership of @path.
 */
void fsinfo_update_cwd(struct file_system_info *info, struct fs_path *path);

/**
 * fsinfo_update_root() - Update the root directory of the file system info
 * @info: The file system info to update the root directory for
 * @path: The new root directory
 *
 * Puts the old root directory and takes ownership of @path.
 */
void fsinfo_update_root(struct file_system_info *info, struct fs_path *path);

/**
 * fsinfo_get_cwd() - Get the current working directory of the file system info
 * @info: The file system info to get the current working directory for
 * @path: The buffer to store the current working directory
 *
 * This function increments the reference count of the current working directory.
 * The caller must call fs_path_put() on the returned path after use.
 */
void fsinfo_get_cwd(struct file_system_info *info, struct fs_path *path);

/**
 * fsinfo_get_root() - Get the root directory of the file system info
 * @info: The file system info to get the root directory for
 * @path: The buffer to store the root directory
 *
 * This function increments the reference count of the root directory.
 * The caller must call fs_path_put() on the returned path after use.
 */
void fsinfo_get_root(struct file_system_info *info, struct fs_path *path);

void fsinfo_free_resources(struct file_system_info *info);

#endif
