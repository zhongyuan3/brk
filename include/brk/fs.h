#ifndef BRK_FS_H
#define BRK_FS_H

#include <brk/fs_types.h>
#include <brk/lock.h>
#include <brk/path.h>
#include <brk/refcnt.h>
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

/*
 * VFS contracts
 * -------------
 * 1) Pointer-returning helpers should follow a single error model:
 *      success: valid non-NULL pointer
 *      failure: ERR_PTR(-errno)
 *    Use bare NULL only when the API explicitly defines a non-error
 *    "not found" outcome.
 *
 * 2) "no extra reference count" comments are invariants:
 *      - inode lifetime is bounded by superblock lifetime
 *      - file->f_inode is pinned indirectly by file->f_path.dentry
 *    If these assumptions change, add explicit get/put pairs.
 *
 * 3) Lock ordering guideline:
 *      file_systems_lock
 *        -> file_system_type.lock
 *          -> super_block.mnt_states_lock
 *            -> mount.mnt_lock
 *      inode_hash_lock -> inode.lock
 *      dentry_htable_lock -> dentry.d_lock -> inode.lock
 *      super_block.inodes_lock -> inode.lock
 *
 *    Keep global spinlock critical sections short. Avoid calling filesystem
 *    callbacks while holding global hash/table spinlocks.
 */

struct fs_driver {
	const char *name;
	unsigned int flags;

	/**
	 * mount() - mount this filesystem and return the root dentry
	 * @fs_type: this filesystem type
	 * @flags: mount flags (%MS_RDONLY, etc.)
	 * @dev_name: device path or special name
	 * @data: filesystem-private mount options string
	 *
	 * Responsibility boundary (constructor side):
	 *   1) Allocate and initialize a new superblock (typically fs_super_block_alloc()).
	 *   2) Fill core sb fields at least:
	 *        - driver/ops/flags/block_size/magic
	 *   3) Allocate filesystem-private state and store it in @sb->private_data.
	 *   4) Build root inode and root dentry, then set @sb->root.
	 *   5) Link @sb into @fs_type->super_blocks list.
	 *
	 * Ownership after successful return:
	 *   - returned root dentry reference is transferred to VFS mount code
	 *   - superblock lifetime is finalized by ->kill_sb() on unmount
	 *
	 * Error path:
	 *   unwind all partially allocated resources before returning
	 *   ERR_PTR(-errno); do not leak sb/private/root references.
	 *
	 * Return uses transfer semantics. If filesystem code needs to retain the
	 * root dentry after returning, it must take an extra fs_dentry_get().
	 *
	 * Note: dentry->d_sb is a raw pointer and does not retain superblock
	 * lifetime by itself.
	 *
	 * Return: root dentry with reference held, or ERR_PTR(-errno) on failure.
	 */
	struct fs_dentry *(*mount)(struct fs_driver *fs_type, int flags,
				   const char *dev_name, void *data);

	/**
	 * kill_sb() - destroy superblock (unmount filesystem)
	 * @sb: superblock to destroy
	 *
	 * Responsibility boundary (destructor side):
	 *   1) Detach @sb from @fs_type->super_blocks list.
	 *   2) Drop the superblock reference (typically fs_super_block_put(sb)); ->put_super()
	 *      runs when count reaches zero.
	 *   3) Filesystem-specific teardown belongs in ->put_super():
	 *        - free @sb->private_data
	 *        - drop @sb->root reference(s)
	 *        - release remaining fs-private allocations.
	 *
	 * Contract with mount core:
	 *   - mount topology/hash links are removed before ->kill_sb()
	 *   - ->kill_sb() handles only superblock/filesystem teardown, not mount
	 *     namespace topology.
	 */
	void (*kill_sb)(struct fs_super_block *sb);

	spinlock_t lock;
	struct list_head super_blocks; /* protected by lock */

	struct list_head list; /* protected by file_systems_lock */
};

struct fs_super_block {
	struct list_head list; /* protected by sb_lock */
	struct fs_driver *driver;
	struct list_head instance; /* protected by driver->lock */
	refcnt_t count;

	unsigned long block_size;
	unsigned long magic;
	unsigned long flags;
	const struct fs_super_block_ops *ops;

	struct fs_dentry *root; /* hold ref count */

	void *private_data;

	spinlock_t inodes_lock;
	struct list_head inodes; /* protected by inodes_lock */
	struct list_head dirty_inodes; /* protected by inodes_lock */

	spinlock_t mnt_states_lock;
	struct list_head mnt_states; /* protected by mnt_states_lock */

	const struct fs_dentry_ops *default_dops; /* default d_op for dentries */
};

struct fs_super_block_ops {
	/**
	 * alloc_inode() - allocate and partially initialize an inode
	 * @sb: owning superblock
	 *
	 * If the filesystem needs a private struct after the inode, allocate a
	 * single object here and return a pointer to the VFS inode part. If this
	 * method is %NULL, the VFS allocates a generic inode from a kmem cache.
	 *
	 * Return: new inode, or %NULL on failure.
	 */
	struct fs_inode *(*alloc_inode)(struct fs_super_block *sb);

	/**
	 * free_inode() - free memory from alloc_inode()
	 * @inode: inode with zero refcount, removed from the inode hash
	 *
	 * Called when the inode leaves the cache; evict_inode() has already run.
	 */
	void (*free_inode)(struct fs_inode *inode);

	/**
	 * dirty_inode() - notify filesystem of dirty inode metadata
	 * @inode: inode that became dirty
	 * @flags: %I_DIRTY_SYNC, %I_DIRTY_DATASYNC, etc.
	 *
	 * Optional; used for journaling or internal bookkeeping.
	 */
	void (*dirty_inode)(struct fs_inode *inode, int flags);

	/**
	 * write_inode() - write inode metadata to disk
	 * @inode: inode to write back
	 * @sync: non-zero to wait for I/O completion
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*write_inode)(struct fs_inode *inode, int sync);

	/**
	 * evict_inode() - inode is about to be evicted from memory
	 * @inode: inode to evict (refcount already zero)
	 *
	 * Truncate file data if i_nlink == 0, release on-disk inode, then call
	 * fs_inode_clear(). If %NULL, the VFS uses generic handling.
	 */
	void (*evict_inode)(struct fs_inode *inode);

	/**
	 * put_super() - superblock is about to be freed
	 * @sb: superblock being torn down
	 *
	 * Release @private_data and similar. Called when @count reaches zero.
	 */
	void (*put_super)(struct fs_super_block *sb);

	/**
	 * sync_fs() - sync all dirty filesystem state to the device
	 * @sb: superblock
	 * @wait: non-zero to wait for all I/O
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*sync_fs)(struct fs_super_block *sb, int wait);
};

struct fs_inode {
	/*
	 * No extra refcount is held on sb by inode itself.
	 * Contract: superblock must outlive all inodes attached to it.
	 */
	struct fs_super_block *sb;
	const struct fs_inode_ops *ops;
	const struct fs_file_ops *fops;

	refcnt_t count;
	spinlock_t lock; /* protects state and dentries linkage */
	sleeplock_t
		rwsem; /* VFS op serialization: read for lookup, write for mutate */
	unsigned long state; /* protected by lock */

	struct list_head sb_list; /* protected by sb->inodes_lock */
	struct list_head list; /* protected by sb->inodes_lock */

	struct hlist_node hash; /* protected by inode_hash_lock */

	struct list_head dentries; /* protected by lock */

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

	/*
	 * Page cache for regular files (and any inode whose backing store can
	 * be addressed in PAGE_SIZE units). May be %NULL for inodes that have
	 * no associated data, e.g. directories, devices, pipes.
	 *
	 * Lifetime: allocated by the filesystem (typically via
	 * fs_inode_attach_page_cache()) once the file type is known; freed by the
	 * VFS in fs_inode_put() after ->evict_inode() returns.
	 */
	struct page_cache *mapping;

	void *private_data;
};

struct fs_inode_ops {
	/**
	 * lookup() - look up name in directory
	 * @dir: parent directory inode (rwsem read held)
	 * @dentry: negative dentry (d_inode %NULL)
	 * @flags: %LOOKUP_* flags
	 *
	 * Load the inode from backing store and associate with fs_dentry_splice_alias().
	 *
	 * Return: %NULL if not found, ERR_PTR() if error, a dentry if found.
	 */
	struct fs_dentry *(*lookup)(struct fs_inode *dir,
				    struct fs_dentry *dentry,
				    unsigned int flags);

	/**
	 * create() - create regular file
	 * @dir: parent directory (rwsem write held)
	 * @dentry: target negative dentry
	 * @mode: file mode
	 * @excl: require exclusive creation
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*create)(struct fs_inode *dir, struct fs_dentry *dentry,
		      umode_t mode, bool excl);

	/**
	 * link() - create hard link
	 * @old_dentry: source file
	 * @dir: target directory (rwsem write held)
	 * @new_dentry: negative dentry for new link
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*link)(struct fs_dentry *old_dentry, struct fs_inode *dir,
		    struct fs_dentry *new_dentry);

	/**
	 * unlink() - remove directory entry
	 * @dir: parent directory (rwsem write held)
	 * @dentry: dentry to remove
	 *
	 * Decrement i_nlink; if it reaches zero with no open references, data is
	 * removed in evict_inode().
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*unlink)(struct fs_inode *dir, struct fs_dentry *dentry);

	/**
	 * symlink() - create symbolic link
	 * @dir: parent directory (rwsem write held)
	 * @dentry: negative dentry for new symlink
	 * @symname: link target string
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*symlink)(struct fs_inode *dir, struct fs_dentry *dentry,
		       const char *symname);

	/**
	 * readlink() - read symlink target into user buffer
	 * @dentry: symlink dentry
	 * @buf: user buffer
	 * @bufsiz: buffer size
	 *
	 * Return: number of bytes copied, or negative errno.
	 */
	int (*readlink)(struct fs_dentry *dentry, char *buf, int bufsiz);

	/**
	 * mkdir() - create subdirectory
	 * @dir: parent directory (rwsem write held)
	 * @dentry: negative dentry for new directory
	 * @mode: directory mode
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*mkdir)(struct fs_inode *dir, struct fs_dentry *dentry,
		     umode_t mode);

	/**
	 * rmdir() - remove empty directory
	 * @dir: parent directory (rwsem write held)
	 * @dentry: dentry to remove
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*rmdir)(struct fs_inode *dir, struct fs_dentry *dentry);

	/**
	 * rename() - rename or move
	 * @old_dir: source directory (rwsem write held)
	 * @old_dentry: source dentry
	 * @new_dir: target directory (rwsem write held)
	 * @new_dentry: target dentry
	 * @flags: %RENAME_* flags
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*rename)(struct fs_inode *old_dir, struct fs_dentry *old_dentry,
		      struct fs_inode *new_dir, struct fs_dentry *new_dentry,
		      unsigned int flags);

	/**
	 * mknod() - create device node, fifo, etc.
	 * @dir: parent directory (rwsem write held)
	 * @dentry: negative dentry
	 * @mode: type and permissions
	 * @dev: device number when applicable
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*mknod)(struct fs_inode *dir, struct fs_dentry *dentry,
		     umode_t mode, dev_t dev);

	/**
	 * getattr() - get inode attributes
	 * @path: file path
	 * @stat: output buffer
	 * @mask: requested attribute mask
	 * @flags: query flags
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*getattr)(const struct fs_path *path, struct stat *stat, u32 mask,
		       unsigned int flags);

	/**
	 * setattr() - set inode attributes
	 * @dentry: target dentry
	 * @attr: attributes to apply
	 *
	 * Caller must hold rwsem write on the inode.
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*setattr)(struct fs_dentry *dentry, struct fs_iattr *attr);
};

struct fs_file {
	refcnt_t count;

	struct fs_path
		path; /* open path (dentry and mount); holds refs via fs_path_get/fs_path_put */
	struct fs_inode *
		inode; /* cached from f_path.dentry->d_inode; no extra inode refcount */
	const struct fs_file_ops *ops; /* file operations; fixed after open */

	spinlock_t lock;
	fmode_t mode; /* %FMODE_READ | %FMODE_WRITE, ...; protected by @f_lock */

	sleeplock_t pos_lock; /* serializes f_pos updates with read/write/lseek */
	loff_t pos; /* current file offset; protected by @f_pos_lock */

	void *private_data; /* filesystem private data; open allocates, release frees */
};

struct fs_file_ops {
	/**
	 * open() - open file
	 * @inode: file inode
	 * @file: file object being initialized
	 *
	 * May allocate file->private_data.
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*open)(struct fs_inode *inode, struct fs_file *file);

	/**
	 * release() - last reference dropped
	 * @inode: file inode
	 * @file: file being freed
	 *
	 * Release private_data.
	 *
	 * Return: %0 (often ignored).
	 */
	int (*release)(struct fs_inode *inode, struct fs_file *file);

	/**
	 * read() - read from file
	 * @file: file object
	 * @buf: user buffer
	 * @size: byte count
	 * @pos: file position (updated)
	 *
	 * Return: bytes read, %0 at EOF, or negative errno.
	 */
	ssize_t (*read)(struct fs_file *file, char *buf, usize_t size,
			loff_t *pos);

	/**
	 * write() - write to file
	 * @file: file object
	 * @buf: user buffer
	 * @size: byte count
	 * @pos: file position (updated)
	 *
	 * Return: bytes written, or negative errno.
	 */
	ssize_t (*write)(struct fs_file *file, const char *buf, usize_t size,
			 loff_t *pos);

	/**
	 * llseek() - reposition read/write offset
	 * @file: file object
	 * @offset: offset argument
	 * @whence: %SEEK_SET, %SEEK_CUR, or %SEEK_END
	 *
	 * Return: new offset, or negative errno.
	 */
	loff_t (*llseek)(struct fs_file *file, loff_t offset, int whence);

	/**
	 * iterate_shared() - iterate directory entries
	 * @file: directory file
	 * @ctx: directory context
	 *
	 * Emit entries with dir_emit().
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*iterate_shared)(struct fs_file *file,
			      struct fs_dir_iterator *ctx);

	/**
	 * fsync() - flush file data and/or metadata
	 * @file: file object
	 * @start: range start
	 * @end: range end
	 * @datasync: non-zero for data-only sync
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*fsync)(struct fs_file *file, loff_t start, loff_t end,
		     int datasync);

	/**
	 * flush() - called on every close()
	 * @file: file object
	 *
	 * Unlike release(), runs once per close, not only on last fput.
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*flush)(struct fs_file *file);

	/**
	 * ioctl() - device control
	 * @file: file object
	 * @cmd: command code
	 * @arg: argument
	 *
	 * Return: command-specific value, or negative errno.
	 */
	long (*ioctl)(struct fs_file *file, unsigned int cmd,
		      unsigned long arg);
};

int fs_driver_register(struct fs_driver *fs);
int fs_driver_unregister(struct fs_driver *fs);
struct fs_driver *fs_driver_lookup(const char *name);

/*
 * Iterate every registered filesystem and invoke @fn with the global
 * filesystem list lock held. @fn must be non-blocking (no allocation,
 * no sleeplock acquisition).
 */
void fs_driver_for_each(void (*fn)(const struct fs_driver *fs, void *ctx),
			void *ctx);

struct fs_super_block *fs_super_block_alloc(struct fs_driver *driver);
void fs_super_block_free(struct fs_super_block *sb);
void fs_super_block_get(struct fs_super_block *sb);
void fs_super_block_put(struct fs_super_block *sb);

struct fs_inode *fs_inode_get_locked(struct fs_super_block *sb,
				     unsigned long ino);
void fs_inode_unlock_new(struct fs_inode *inode);
struct fs_inode *fs_inode_get(struct fs_inode *inode);
void fs_inode_put(struct fs_inode *inode);
void fs_inode_mark_dirty(struct fs_inode *inode);
void fs_inode_clear(struct fs_inode *inode);
void fs_inode_cache_init(void);
int fs_inode_attach_page_cache(struct fs_inode *inode,
			       const struct page_cache_ops *a_ops);

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
