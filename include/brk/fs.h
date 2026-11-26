#ifndef _BRK_FS_H
#define _BRK_FS_H

#include <brk/fs_types.h>
#include <brk/mutex_types.h>
#include <brk/path.h>
#include <brk/refcnt_types.h>
#include <brk/rwsem_types.h>
#include <brk/stat_types.h>
#include <brk/types.h>
#include <uapi/fcntl.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Internal rename flags; not exposed through the syscall ABI anymore. */
#define RENAME_NOREPLACE (1 << 0)
#define RENAME_EXCHANGE (1 << 1)
#define RENAME_WHITEOUT (1 << 2)

#define FS_REQUIRES_DEV 1
#define FS_BINARY_MOUNTDATA 2
#define FS_USERNS_MOUNT 4

#define INODE_HTABLE_BITS 8
#define INODE_HTABLE_SIZE (1 << INODE_HTABLE_BITS)

#define I_DIRTY_SYNC (1 << 0)
#define I_DIRTY_DATASYNC (1 << 1)
#define I_DIRTY_PAGES (1 << 2)
#define I_NEW (1 << 3)
#define I_WILL_FREE (1 << 4)
#define I_FREEING (1 << 5)
#define I_CLEAR (1 << 6)
#define I_BAD (1 << 7) /* I_NEW initialization failed; do not evict */
#define I_DIRTY (I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_PAGES)

typedef unsigned int fmode_t;

#define FMODE_READ (1 << 0)
#define FMODE_WRITE (1 << 1)
#define FMODE_DIR (1 << 2)

struct page_cache;
struct page_cache_ops;

struct mount_args {
	struct file_system_type *type;
	const char *dev_name;
	void *data;
	unsigned long flags;
};

struct mount_result {
	struct super_block *sb;
	struct dentry *root;
};

struct file_system_type {
	const char *name;
	unsigned int flags;
	int (*mount)(struct mount_args *, struct mount_result *);
	spinlock_t lock;
	struct list_head super_blocks;
	struct list_head list;
};

struct super_block {
	struct list_head list;
	struct file_system_type *type;
	struct list_head instance;
	refcnt_t count;
	unsigned long block_size;
	unsigned long magic;
	unsigned long flags;
	const struct super_block_ops *ops;
	struct dentry *root;
	void *private_data;
	spinlock_t inodes_lock;
	struct list_head inodes;
	struct list_head dirty_inodes;
	spinlock_t mnt_states_lock;
	struct list_head mnt_states;
	const struct dentry_ops *default_dops;
};

struct super_block_ops {
	struct inode *(*alloc_inode)(struct super_block *);
	void (*free_inode)(struct inode *);
	int (*write_inode)(struct inode *, int);
	void (*evict_inode)(struct inode *);
	void (*put_super)(struct super_block *);
	/*
	 * Optional: flush filesystem-level metadata that is not attached to
	 * an inode (block/inode bitmaps, directory blocks, ...).  Called by
	 * sync_filesystem() after all dirty inode writebacks have completed.
	 */
	int (*sync_fs)(struct super_block *);
	int (*statfs)(struct super_block *, struct kstatfs *);
};

struct inode {
	struct super_block *sb;
	const struct inode_ops *ops;
	const struct file_ops *fops;
	refcnt_t count;
	spinlock_t lock;
	rwsem_t rwsem;
	unsigned long state;
	struct list_head sb_list;
	struct list_head list;
	struct hlist_node hash;
	struct list_head dentries;
	unsigned long ino;
	umode_t mode;
	unsigned int nlink;
	loff_t size;
	dev_t rdev;
	ktime_t atime;
	ktime_t mtime;
	ktime_t ctime;
	kgid_t gid;
	kuid_t uid;
	struct page_cache *mapping;
	void *private_data;
};

struct inode_ops {
	struct dentry *(*lookup)(struct inode *, struct dentry *, unsigned int);
	int (*create)(struct inode *, struct dentry *, umode_t, bool);
	int (*link)(struct dentry *, struct inode *, struct dentry *);
	int (*unlink)(struct inode *, struct dentry *);
	int (*symlink)(struct inode *, struct dentry *, const char *);
	int (*readlink)(struct dentry *, char *, int);
	int (*mkdir)(struct inode *, struct dentry *, umode_t);
	int (*rmdir)(struct inode *, struct dentry *);
	int (*rename)(struct inode *, struct dentry *, struct inode *,
		      struct dentry *, unsigned int);
	int (*mknod)(struct inode *, struct dentry *, umode_t, dev_t);
	int (*getattr)(const struct path *, struct kstat *, u32, unsigned int);
	int (*setattr)(struct dentry *, struct iattr *);
};

struct file {
	refcnt_t count;
	struct path path;
	struct inode *inode;
	const struct file_ops *ops;
	spinlock_t lock;
	fmode_t mode;
	/* Anonymous object (socket, ...): no path/inode to release. */
	bool anon;
	mutex_t pos_lock;
	loff_t pos;
	/* Driver-specific per-open state (e.g. tty). */
	void *private_data;
	/* Class-core per-open state (e.g. char_dev reference). */
	void *dev_private;
};

struct file_ops {
	int (*open)(struct inode *, struct file *);
	int (*release)(struct inode *, struct file *);
	ssize_t (*read)(struct file *, char *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
	loff_t (*llseek)(struct file *, loff_t, int);
	int (*iterate_shared)(struct file *, struct dir_iterator *);
	int (*fsync)(struct file *, loff_t, loff_t, int);
	int (*flush)(struct file *);
	long (*ioctl)(struct file *, unsigned int, unsigned long);
	int (*fallocate)(struct file *, int mode, loff_t offset, loff_t len);
	/* Readiness for poll/select; returns POLL* revents. */
	unsigned int (*poll)(struct file *file, unsigned int events);
};

int file_system_type_register(struct file_system_type *fs);
int file_system_type_unregister(struct file_system_type *fs);
struct file_system_type *file_system_type_lookup(const char *name);
void file_system_type_for_each(void (*fn)(const struct file_system_type *fs,
					  void *ctx),
			       void *ctx);

struct super_block *super_block_alloc(struct file_system_type *type);
void super_block_free(struct super_block *sb);
void super_block_get(struct super_block *sb);
void super_block_put(struct super_block *sb);

/*
 * sync_filesystem() - write back every dirty inode of @sb (data + metadata).
 * sync_all_filesystems() - run sync_filesystem() over all mounted filesystems.
 *
 * Return: 0 on success, or the first negative errno encountered.
 */
int sync_filesystem(struct super_block *sb);
int sync_all_filesystems(void);

struct inode *inode_get_locked(struct super_block *sb, unsigned long ino);
void inode_unlock_new(struct inode *inode);
void inode_fail_new(struct inode *inode);
struct inode *inode_get(struct inode *inode);
void inode_put(struct inode *inode);
void inode_mark_dirty(struct inode *inode);
void inode_clear(struct inode *inode);
void inode_cache_init(void);
int inode_attach_page_cache(struct inode *inode,
			    const struct page_cache_ops *ops);
void inode_times_set_all_now(struct inode *inode);
void inode_times_set(struct inode *inode, const ktime_t *atime,
		     const ktime_t *mtime);
void inode_touch_mtime(struct inode *inode);
void inode_touch_ctime(struct inode *inode);
void inode_touch_mtime_ctime(struct inode *inode);
void inode_times_to_kstat(struct inode *inode, struct kstat *ks);

struct file *file_alloc(struct path *path, fmode_t mode);
struct file *file_alloc_anon(const struct file_ops *ops, void *private_data);
struct file *file_get(struct file *file);
void file_put(struct file *file);
loff_t file_lseek(struct file *file, loff_t len, int whence);
ssize_t file_read(struct file *file, void *buf, size_t size);
ssize_t file_write(struct file *file, const void *buf, size_t size);
long file_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
unsigned int file_poll(struct file *file, unsigned int events);
int file_stat(struct file *file, struct kstat *buf);
void remove_inode_privs(struct inode *inode, struct dentry *dentry);
void file_remove_privs(struct file *file);
int statfs(struct super_block *sb, struct kstatfs *buf);
int file_truncate(struct file *file, loff_t size);
int file_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
void file_cache_init(void);

struct file *do_openat(int dirfd, const char *path, int flags, umode_t mode);
int do_execve(const char *path, char **argv, char **envp);
int do_mkdirat(int dirfd, const char *path, umode_t mode);
int do_mknodat(int dirfd, const char *path, umode_t mode, dev_t dev);
int do_linkat(int olddirfd, const char *oldpath, int newdirfd,
	      const char *newpath, int flags);
int do_unlinkat(int dirfd, const char *path, int flags);
int do_symlinkat(int dirfd, const char *pathname, const char *target);
int do_readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);
int do_renameat(int olddirfd, const char *oldpath, int newdirfd,
		const char *newpath, unsigned int flags);

int vfs_init(void);
int rootfs_init(void);

void pipe_fs_init(void);
int anon_pipe_create(struct file **read_file, struct file **write_file,
		     unsigned int flags);

int do_pipe2(int *pipefd, int flags);

extern struct file_system_type tmpfs_fs_type;
extern struct file_system_type brkfs_fs_type;
extern struct file_system_type procfs_fs_type;

extern const struct super_block_ops tmpfs_sops;
extern const struct inode_ops tmpfs_iops;
extern const struct file_ops tmpfs_dir_fops;
extern const struct file_ops tmpfs_file_fops;

extern const struct super_block_ops brkfs_sops;
extern const struct inode_ops brkfs_iops;
extern const struct file_ops brkfs_dir_fops;
extern const struct file_ops brkfs_file_fops;

extern const struct file_ops chrdev_fops;
extern const struct file_ops blkdev_fops;

#endif
