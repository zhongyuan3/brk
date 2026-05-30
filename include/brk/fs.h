#ifndef BRK_FS_H
#define BRK_FS_H

#include <brk/fs_types.h>
#include <brk/path.h>
#include <brk/refcnt_types.h>
#include <brk/sleeplock_types.h>
#include <brk/types.h>
#include <uapi/stat.h>
#include <uapi/time.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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
#define I_DIRTY (I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_PAGES)

#define FMODE_READ (1 << 0)
#define FMODE_WRITE (1 << 1)
#define FMODE_DIR (1 << 2)

struct page_cache;
struct page_cache_ops;

struct fs_mount_args {
	struct fs_driver *driver;
	const char *dev_name;
	void *data;
	unsigned long flags;
};

struct fs_mount_result {
	struct fs_super_block *sb;
	struct fs_dentry *root;
};

struct fs_driver {
	const char *name;
	unsigned int flags;
	int (*mount)(struct fs_mount_args *, struct fs_mount_result *);
	spinlock_t lock;
	struct list_head super_blocks;
	struct list_head list;
};

struct fs_super_block {
	struct list_head list;
	struct fs_driver *driver;
	struct list_head instance;
	refcnt_t count;
	unsigned long block_size;
	unsigned long magic;
	unsigned long flags;
	const struct fs_super_block_ops *ops;
	struct fs_dentry *root;
	void *private_data;
	spinlock_t inodes_lock;
	struct list_head inodes;
	struct list_head dirty_inodes;
	spinlock_t mnt_states_lock;
	struct list_head mnt_states;
	const struct fs_dentry_ops *default_dops;
};

struct fs_super_block_ops {
	struct fs_inode *(*alloc_inode)(struct fs_super_block *);
	void (*free_inode)(struct fs_inode *);
	int (*write_inode)(struct fs_inode *, int);
	void (*evict_inode)(struct fs_inode *);
	void (*put_super)(struct fs_super_block *);
};

struct fs_inode {
	struct fs_super_block *sb;
	const struct fs_inode_ops *ops;
	const struct fs_file_ops *fops;
	refcnt_t count;
	spinlock_t lock;
	sleeplock_t rwsem;
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
	struct timespec atime;
	struct timespec mtime;
	struct timespec ctime;
	kgid_t gid;
	kuid_t uid;
	struct page_cache *mapping;
	void *private_data;
};

struct fs_inode_ops {
	struct fs_dentry *(*lookup)(struct fs_inode *, struct fs_dentry *,
				    unsigned int);
	int (*create)(struct fs_inode *, struct fs_dentry *, umode_t, bool);
	int (*link)(struct fs_dentry *, struct fs_inode *, struct fs_dentry *);
	int (*unlink)(struct fs_inode *, struct fs_dentry *);
	int (*symlink)(struct fs_inode *, struct fs_dentry *, const char *);
	int (*readlink)(struct fs_dentry *, char *, int);
	int (*mkdir)(struct fs_inode *, struct fs_dentry *, umode_t);
	int (*rmdir)(struct fs_inode *, struct fs_dentry *);
	int (*rename)(struct fs_inode *, struct fs_dentry *, struct fs_inode *,
		      struct fs_dentry *, unsigned int);
	int (*mknod)(struct fs_inode *, struct fs_dentry *, umode_t, dev_t);
	int (*getattr)(const struct fs_path *, struct stat *, u32,
		       unsigned int);
	int (*setattr)(struct fs_dentry *, struct fs_iattr *);
};

struct fs_file {
	refcnt_t count;
	struct fs_path path;
	struct fs_inode *inode;
	const struct fs_file_ops *ops;
	spinlock_t lock;
	fmode_t mode;
	sleeplock_t pos_lock;
	loff_t pos;
	void *private_data;
};

struct fs_file_ops {
	int (*open)(struct fs_inode *, struct fs_file *);
	int (*release)(struct fs_inode *, struct fs_file *);
	ssize_t (*read)(struct fs_file *, char *, usize_t, loff_t *);
	ssize_t (*write)(struct fs_file *, const char *, usize_t, loff_t *);
	loff_t (*llseek)(struct fs_file *, loff_t, int);
	int (*iterate_shared)(struct fs_file *, struct fs_dir_iterator *);
	int (*fsync)(struct fs_file *, loff_t, loff_t, int);
	int (*flush)(struct fs_file *);
	long (*ioctl)(struct fs_file *, unsigned int, unsigned long);
};

int fs_driver_register(struct fs_driver *fs);
int fs_driver_unregister(struct fs_driver *fs);
struct fs_driver *fs_driver_lookup(const char *name);
void fs_driver_for_each(void (*fn)(const struct fs_driver *fs, void *ctx),
			void *ctx);

struct fs_super_block *fs_super_block_alloc(struct fs_driver *driver);
void fs_super_block_free(struct fs_super_block *sb);
void fs_super_block_get(struct fs_super_block *sb);
void fs_super_block_put(struct fs_super_block *sb);

/*
 * sync_filesystem() - write back every dirty inode of @sb (data + metadata).
 * sync_all_filesystems() - run sync_filesystem() over all mounted filesystems.
 *
 * Return: 0 on success, or the first negative errno encountered.
 */
int sync_filesystem(struct fs_super_block *sb);
int sync_all_filesystems(void);

struct fs_inode *fs_inode_get_locked(struct fs_super_block *sb,
				     unsigned long ino);
void fs_inode_unlock_new(struct fs_inode *inode);
struct fs_inode *fs_inode_get(struct fs_inode *inode);
void fs_inode_put(struct fs_inode *inode);
void fs_inode_mark_dirty(struct fs_inode *inode);
void fs_inode_clear(struct fs_inode *inode);
void fs_inode_cache_init(void);
int fs_inode_attach_page_cache(struct fs_inode *inode,
			       const struct page_cache_ops *ops);

struct fs_file *fs_file_alloc(struct fs_path *path, fmode_t mode);
struct fs_file *fs_file_get(struct fs_file *file);
void fs_file_put(struct fs_file *file);
loff_t fs_file_lseek(struct fs_file *file, loff_t len, int whence);
ssize_t fs_file_read(struct fs_file *file, void *buf, usize_t size);
ssize_t fs_file_write(struct fs_file *file, const void *buf, usize_t size);
long fs_file_ioctl(struct fs_file *file, unsigned int cmd, unsigned long arg);
int fs_file_stat(struct fs_file *file, struct stat *buf);
int fs_file_truncate(struct fs_file *file, loff_t size);
void fs_file_cache_init(void);

struct fs_file *do_openat(int dirfd, const char *path, int flags, umode_t mode);
int do_execve(const char *path, char **argv, char **envp);
int do_mkdirat(int dirfd, const char *path, umode_t mode);
int do_mknodat(int dirfd, const char *path, umode_t mode, dev_t dev);
int do_linkat(int olddirfd, const char *oldpath, int newdirfd,
	      const char *newpath, int flags);
int do_unlinkat(int dirfd, const char *path, int flags);
int do_symlinkat(int dirfd, const char *pathname, const char *target);
int do_readlinkat(int dirfd, const char *path, char *buf, usize_t bufsiz);
int do_creat(const char *pathname, umode_t mode);
int do_renameat(int olddirfd, const char *oldpath, int newdirfd,
		const char *newpath, unsigned int flags);
int do_rmdir(const char *pathname);

int fs_init(void);

void pipe_fs_init(void);
int anon_pipe_create(struct fs_file **read_file, struct fs_file **write_file,
		     unsigned int flags);

int do_pipe2(int *pipefd, int flags);

extern struct fs_driver tmpfs_fs_type;
extern struct fs_driver brkfs_fs_type;
extern struct fs_driver procfs_fs_type;

extern const struct fs_super_block_ops tmpfs_sops;
extern const struct fs_inode_ops tmpfs_iops;
extern const struct fs_file_ops tmpfs_dir_fops;
extern const struct fs_file_ops tmpfs_file_fops;

extern const struct fs_super_block_ops brkfs_sops;
extern const struct fs_inode_ops brkfs_iops;
extern const struct fs_file_ops brkfs_dir_fops;
extern const struct fs_file_ops brkfs_file_fops;

extern const struct fs_file_ops chrdev_fops;
extern const struct fs_file_ops blkdev_fops;

#endif
