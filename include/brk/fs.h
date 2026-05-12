#ifndef BRK_FS_H
#define BRK_FS_H

#include <brk/fs_types.h>
#include <brk/lock.h>
#include <brk/path.h>
#include <brk/refcnt.h>
#include <brk/stat.h>
#include <brk/time.h>
#include <brk/types.h>

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
 *        -> file_system_type.fs_lock
 *          -> super_block.s_mount_lock
 *            -> mount.mnt_lock
 *      inode_hash_lock -> inode.i_lock
 *      dentry_htable_lock -> dentry.d_lock -> inode.i_lock
 *      super_block.s_inode_lock -> inode.i_lock
 *
 *    Keep global spinlock critical sections short. Avoid calling filesystem
 *    callbacks while holding global hash/table spinlocks.
 */

struct file_system_type {
	const char *name;
	int fs_flags;

	/**
	 * mount() - mount this filesystem and return the root dentry
	 * @fs_type: this filesystem type
	 * @flags: mount flags (%MS_RDONLY, etc.)
	 * @dev_name: device path or special name
	 * @data: filesystem-private mount options string
	 *
	 * Responsibility boundary (constructor side):
	 *   1) Allocate and initialize a new superblock (typically alloc_super()).
	 *   2) Fill core sb fields at least:
	 *        - s_type/s_op/s_flags/s_blocksize/s_magic
	 *   3) Allocate filesystem-private state and store it in @sb->s_fs_info.
	 *   4) Build root inode and root dentry, then set @sb->s_root.
	 *   5) Link @sb into @fs_type->fs_supers list.
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
	 * root dentry after returning, it must take an extra dentry_dup().
	 *
	 * Note: dentry->d_sb is a raw pointer and does not retain superblock
	 * lifetime by itself.
	 *
	 * Return: root dentry with reference held, or ERR_PTR(-errno) on failure.
	 */
	struct dentry *(*mount)(struct file_system_type *fs_type, int flags,
				const char *dev_name, void *data);

	/**
	 * kill_sb() - destroy superblock (unmount filesystem)
	 * @sb: superblock to destroy
	 *
	 * Responsibility boundary (destructor side):
	 *   1) Detach @sb from @fs_type->fs_supers list.
	 *   2) Drop the superblock reference (typically super_put(sb)); ->put_super()
	 *      runs when s_count reaches zero.
	 *   3) Filesystem-specific teardown belongs in ->put_super():
	 *        - free @sb->s_fs_info
	 *        - drop @sb->s_root reference(s)
	 *        - release remaining fs-private allocations.
	 *
	 * Contract with mount core:
	 *   - mount topology/hash links are removed before ->kill_sb()
	 *   - ->kill_sb() handles only superblock/filesystem teardown, not mount
	 *     namespace topology.
	 */
	void (*kill_sb)(struct super_block *sb);

	spinlock_t fs_lock;
	struct list_head fs_supers; /* protected by fs_lock */

	struct list_head fs_list; /* protected by file_systems_lock */
};

struct super_block {
	struct list_head s_list; /* protected by sb_lock */
	struct file_system_type *s_type;
	struct list_head s_instances; /* protected by s_type->fs_lock */
	arc_t s_count;

	unsigned long s_blocksize;
	unsigned long s_magic;
	unsigned long s_flags;
	const struct super_operations *s_op;

	struct dentry *s_root; /* hold ref count */

	void *s_fs_info;

	sleeplock_t s_lock; /* superblock-wide sleep lock for slow-path updates */
	spinlock_t s_inode_lock;
	struct list_head s_inodes; /* protected by s_inode_lock */
	struct list_head s_dirty; /* protected by s_inode_lock */

	spinlock_t s_mount_lock;
	struct list_head s_mounts; /* protected by s_mount_lock */

	const struct dentry_operations *s_d_op; /* default d_op for dentries */
};

struct super_operations {
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
	struct inode *(*alloc_inode)(struct super_block *sb);

	/**
	 * free_inode() - free memory from alloc_inode()
	 * @inode: inode with zero refcount, removed from the inode hash
	 *
	 * Called when the inode leaves the cache; evict_inode() has already run.
	 */
	void (*free_inode)(struct inode *inode);

	/**
	 * dirty_inode() - notify filesystem of dirty inode metadata
	 * @inode: inode that became dirty
	 * @flags: %I_DIRTY_SYNC, %I_DIRTY_DATASYNC, etc.
	 *
	 * Optional; used for journaling or internal bookkeeping.
	 */
	void (*dirty_inode)(struct inode *inode, int flags);

	/**
	 * write_inode() - write inode metadata to disk
	 * @inode: inode to write back
	 * @sync: non-zero to wait for I/O completion
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*write_inode)(struct inode *inode, int sync);

	/**
	 * evict_inode() - inode is about to be evicted from memory
	 * @inode: inode to evict (refcount already zero)
	 *
	 * Truncate file data if i_nlink == 0, release on-disk inode, then call
	 * inode_clear(). If %NULL, the VFS uses generic handling.
	 */
	void (*evict_inode)(struct inode *inode);

	/**
	 * put_super() - superblock is about to be freed
	 * @sb: superblock being torn down
	 *
	 * Release @s_fs_info and similar. Called when @s_count reaches zero.
	 */
	void (*put_super)(struct super_block *sb);

	/**
	 * sync_fs() - sync all dirty filesystem state to the device
	 * @sb: superblock
	 * @wait: non-zero to wait for all I/O
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*sync_fs)(struct super_block *sb, int wait);
};

struct inode {
	/*
	 * No extra refcount is held on i_sb by inode itself.
	 * Contract: superblock must outlive all inodes attached to it.
	 */
	struct super_block *i_sb;
	const struct inode_operations *i_op;
	const struct file_operations *i_fop;

	arc_t i_count;
	spinlock_t i_lock; /* protects i_state and i_dentry linkage */
	sleeplock_t
		i_rwsem; /* VFS op serialization: read for lookup, write for mutate */
	unsigned long i_state; /* protected by i_lock */

	struct list_head i_sb_list; /* protected by i_sb->s_inode_lock */
	struct list_head i_list; /* protected by i_sb->s_inode_lock */

	struct hlist_node i_hash; /* protected by inode_hash_lock */

	struct list_head i_dentry; /* protected by i_lock */

	unsigned long i_ino;
	umode_t i_mode;
	unsigned int i_nlink;
	loff_t i_size;
	dev_t i_rdev;

	struct timespec i_atime;
	struct timespec i_mtime;
	struct timespec i_ctime;

	gid_t i_gid;
	uid_t i_uid;

	void *i_private;
};

struct inode_operations {
	/**
	 * lookup() - look up name in directory
	 * @dir: parent directory inode (i_rwsem read held)
	 * @dentry: negative dentry (d_inode %NULL)
	 * @flags: %LOOKUP_* flags
	 *
	 * Load the inode from backing store and associate with dentry_splice_alias().
	 *
	 * Return: %NULL if not found, ERR_PTR() if error, a dentry if found.
	 */
	struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry,
				 unsigned int flags);

	/**
	 * create() - create regular file
	 * @dir: parent directory (i_rwsem write held)
	 * @dentry: target negative dentry
	 * @mode: file mode
	 * @excl: require exclusive creation
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*create)(struct inode *dir, struct dentry *dentry, umode_t mode,
		      bool excl);

	/**
	 * link() - create hard link
	 * @old_dentry: source file
	 * @dir: target directory (i_rwsem write held)
	 * @new_dentry: negative dentry for new link
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*link)(struct dentry *old_dentry, struct inode *dir,
		    struct dentry *new_dentry);

	/**
	 * unlink() - remove directory entry
	 * @dir: parent directory (i_rwsem write held)
	 * @dentry: dentry to remove
	 *
	 * Decrement i_nlink; if it reaches zero with no open references, data is
	 * removed in evict_inode().
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*unlink)(struct inode *dir, struct dentry *dentry);

	/**
	 * symlink() - create symbolic link
	 * @dir: parent directory (i_rwsem write held)
	 * @dentry: negative dentry for new symlink
	 * @symname: link target string
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*symlink)(struct inode *dir, struct dentry *dentry,
		       const char *symname);

	/**
	 * readlink() - read symlink target into user buffer
	 * @dentry: symlink dentry
	 * @buf: user buffer
	 * @bufsiz: buffer size
	 *
	 * Return: number of bytes copied, or negative errno.
	 */
	int (*readlink)(struct dentry *dentry, char *buf, int bufsiz);

	/**
	 * mkdir() - create subdirectory
	 * @dir: parent directory (i_rwsem write held)
	 * @dentry: negative dentry for new directory
	 * @mode: directory mode
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*mkdir)(struct inode *dir, struct dentry *dentry, umode_t mode);

	/**
	 * rmdir() - remove empty directory
	 * @dir: parent directory (i_rwsem write held)
	 * @dentry: dentry to remove
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*rmdir)(struct inode *dir, struct dentry *dentry);

	/**
	 * rename() - rename or move
	 * @old_dir: source directory (i_rwsem write held)
	 * @old_dentry: source dentry
	 * @new_dir: target directory (i_rwsem write held)
	 * @new_dentry: target dentry
	 * @flags: %RENAME_* flags
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*rename)(struct inode *old_dir, struct dentry *old_dentry,
		      struct inode *new_dir, struct dentry *new_dentry,
		      unsigned int flags);

	/**
	 * mknod() - create device node, fifo, etc.
	 * @dir: parent directory (i_rwsem write held)
	 * @dentry: negative dentry
	 * @mode: type and permissions
	 * @dev: device number when applicable
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*mknod)(struct inode *dir, struct dentry *dentry, umode_t mode,
		     dev_t dev);

	/**
	 * getattr() - get inode attributes
	 * @path: file path
	 * @stat: output buffer
	 * @mask: requested attribute mask
	 * @flags: query flags
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*getattr)(const struct path *path, struct stat *stat,
		       uint32_t mask, unsigned int flags);

	/**
	 * setattr() - set inode attributes
	 * @dentry: target dentry
	 * @attr: attributes to apply
	 *
	 * Caller must hold i_rwsem write on the inode.
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*setattr)(struct dentry *dentry, struct iattr *attr);
};

struct file {
	arc_t f_count;

	struct path
		f_path; /* open path (dentry and mount); holds refs via path_get/path_put */
	struct inode *
		f_inode; /* cached from f_path.dentry->d_inode; no extra inode refcount */
	const struct file_operations
		*f_op; /* file operations; fixed after open */

	spinlock_t f_lock;
	fmode_t f_mode; /* %FMODE_READ | %FMODE_WRITE, ...; protected by @f_lock */

	sleeplock_t
		f_pos_lock; /* serializes f_pos updates with read/write/lseek */
	loff_t f_pos; /* current file offset; protected by @f_pos_lock */

	void *private_data; /* filesystem private data; open allocates, release frees */
};

struct file_operations {
	/**
	 * open() - open file
	 * @inode: file inode
	 * @file: file object being initialized
	 *
	 * May allocate file->private_data.
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*open)(struct inode *inode, struct file *file);

	/**
	 * release() - last reference dropped
	 * @inode: file inode
	 * @file: file being freed
	 *
	 * Release private_data.
	 *
	 * Return: %0 (often ignored).
	 */
	int (*release)(struct inode *inode, struct file *file);

	/**
	 * read() - read from file
	 * @file: file object
	 * @buf: user buffer
	 * @size: byte count
	 * @pos: file position (updated)
	 *
	 * Return: bytes read, %0 at EOF, or negative errno.
	 */
	ssize_t (*read)(struct file *file, char *buf, size_t size, loff_t *pos);

	/**
	 * write() - write to file
	 * @file: file object
	 * @buf: user buffer
	 * @size: byte count
	 * @pos: file position (updated)
	 *
	 * Return: bytes written, or negative errno.
	 */
	ssize_t (*write)(struct file *file, const char *buf, size_t size,
			 loff_t *pos);

	/**
	 * llseek() - reposition read/write offset
	 * @file: file object
	 * @offset: offset argument
	 * @whence: %SEEK_SET, %SEEK_CUR, or %SEEK_END
	 *
	 * Return: new offset, or negative errno.
	 */
	loff_t (*llseek)(struct file *file, loff_t offset, int whence);

	/**
	 * iterate_shared() - iterate directory entries
	 * @file: directory file
	 * @ctx: directory context
	 *
	 * Emit entries with dir_emit().
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*iterate_shared)(struct file *file, struct dir_context *ctx);

	/**
	 * fsync() - flush file data and/or metadata
	 * @file: file object
	 * @start: range start
	 * @end: range end
	 * @datasync: non-zero for data-only sync
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*fsync)(struct file *file, loff_t start, loff_t end, int datasync);

	/**
	 * flush() - called on every close()
	 * @file: file object
	 *
	 * Unlike release(), runs once per close, not only on last fput.
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*flush)(struct file *file);

	/**
	 * ioctl() - device control
	 * @file: file object
	 * @cmd: command code
	 * @arg: argument
	 *
	 * Return: command-specific value, or negative errno.
	 */
	long (*ioctl)(struct file *file, unsigned int cmd, unsigned long arg);
};

int register_filesystem(struct file_system_type *fs);
int unregister_filesystem(struct file_system_type *fs);
struct file_system_type *get_filesystem(const char *name);

struct super_block *alloc_super(struct file_system_type *type);
void free_super(struct super_block *sb);
void super_dup(struct super_block *sb);
void super_put(struct super_block *sb);

struct inode *inode_get_locked(struct super_block *sb, unsigned long ino);
void inode_unlock_new(struct inode *inode);
struct inode *inode_dup(struct inode *inode);
void inode_put(struct inode *inode);
void inode_mark_dirty(struct inode *inode);
void inode_clear(struct inode *inode);
void inode_cache_init(void);

struct file *file_alloc(struct path *path, fmode_t mode);
struct file *file_dup(struct file *file);
void file_put(struct file *file);
loff_t file_lseek(struct file *file, loff_t len, int whence);
ssize_t file_read(struct file *file, void *buf, size_t size);
ssize_t file_write(struct file *file, const void *buf, size_t size);
long file_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int file_stat(struct file *file, struct stat *buf);
int file_truncate(struct file *file, loff_t size);
void file_cache_init(void);

struct file *do_openat(int dirfd, const char *path, int flags, mode_t mode);
int do_execve(const char *path, char **argv, char **envp);
int do_mkdirat(int dirfd, const char *path, mode_t mode);
int do_mknodat(int dirfd, const char *path, mode_t mode, dev_t dev);
int do_linkat(int olddirfd, const char *oldpath, int newdirfd,
	      const char *newpath, int flags);
int do_unlinkat(int dirfd, const char *path, int flags);
int do_symlinkat(int dirfd, const char *pathname, const char *target);
int do_readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);
int do_creat(const char *pathname, mode_t mode);
int do_renameat(int olddirfd, const char *oldpath, int newdirfd,
		const char *newpath, unsigned int flags);
int do_rmdir(const char *pathname);

int fs_init(void);

void pipe_fs_init(void);
int anon_pipe_create(struct file **read_file, struct file **write_file,
		     unsigned int flags);

extern struct file_system_type tmpfs_fs_type;
extern struct file_system_type brkfs_fs_type;

extern const struct super_operations tmpfs_sops;
extern const struct inode_operations tmpfs_iops;
extern const struct file_operations tmpfs_dir_fops;
extern const struct file_operations tmpfs_file_fops;

extern const struct super_operations brkfs_sops;
extern const struct inode_operations brkfs_iops;
extern const struct file_operations brkfs_dir_fops;
extern const struct file_operations brkfs_file_fops;

extern const struct file_operations chrdev_fops;
extern const struct file_operations blkdev_fops;

#endif
