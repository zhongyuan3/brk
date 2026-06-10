#ifndef BRK_FDTABLE_H
#define BRK_FDTABLE_H

#include <brk/fs/fs_types.h>
#include <brk/kernel/refcnt_types.h>
#include <brk/lib/types.h>
#include <brk/lock/spinlock_types.h>
#include <uapi/brk/limits.h>

struct file_desc_table {
	struct fs_file *files[OPEN_MAX];
	spinlock_t lock;
	refcnt_t refcnt;
};

struct file_desc_table *fdtable_alloc(void);
struct file_desc_table *fdtable_get(struct file_desc_table *table);
void fdtable_put(struct file_desc_table *table);

/**
 * fdtable_copy() - Copy a file descriptor table
 * @dst: The file descriptor table to copy to
 * @src: The file descriptor table to copy from
 *
 * @dst must be empty before calling this function.
 */
void fdtable_copy(struct file_desc_table *dst, struct file_desc_table *src);

/**
 * fdtable_alloc_fd() - Allocate a new file descriptor for the file system file
 * @table: The file descriptor table to allocate the file descriptor for
 * @file: The file system file to allocate the file descriptor for
 *
 * This function takes ownership of the @file.
 *
 * Return: The new file descriptor on success, or an error code on failure.
 */
int fdtable_alloc_fd(struct file_desc_table *table, struct fs_file *file);

/**
 * fdtable_close_fd() - Close a file descriptor for the file system file
 * @table: The file descriptor table to close the file descriptor for
 * @fd: The file descriptor to close
 *
 * This function decrements the reference count of the file system file.
 *
 * Return: 0 on success, or an error code on failure.
 */
int fdtable_close_fd(struct file_desc_table *table, int fd);

/**
 * fdtable_get_file() - Get the file system file for a file descriptor
 * @table: The file descriptor table to get the file system file for
 * @fd: The file descriptor to get the file system file for
 *
 * This function increments the reference count of the file system file.
 * The caller must call fs_file_put() on the returned file after use.
 */
struct fs_file *fdtable_get_file(struct file_desc_table *table, int fd);

/**
 * fdtable_dup_fd() - Duplicate a file descriptor
 * @table: The file descriptor table to duplicate the file descriptor for
 * @fd: The file descriptor to duplicate
 *
 * This function duplicates the file descriptor @fd.
 *
 * Return: The new file descriptor on success, or an error code on failure.
 */
int fdtable_dup_fd(struct file_desc_table *table, int fd);

/**
 * fdtable_dup_fd2() - Duplicate a file descriptor
 * @table: The file descriptor table to duplicate the file descriptor for
 * @oldfd: The file descriptor to duplicate
 * @newfd: The new file descriptor
 *
 * This function duplicates the file descriptor @oldfd to @newfd. If @newfd is
 * already open, it will be closed and the file descriptor will be duplicated.
 *
 * Return: 0 on success, or an error code on failure.
 */
int fdtable_dup_fd2(struct file_desc_table *table, int oldfd, int newfd);

void fdtable_close_all(struct file_desc_table *table);

#endif
