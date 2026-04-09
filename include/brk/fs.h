#ifndef BRK_FS_H
#define BRK_FS_H

#include <brk/dcache.h>
#include <brk/dev.h>
#include <brk/fs_types.h>
#include <brk/lock.h>
#include <brk/stat.h>
#include <brk/time.h>
#include <brk/types.h>

struct file_system_type {
	const char *name;
	void (*deinit_sb)(struct super_block *);
	int (*mount)(const struct file_system_type *, const char *,
		     const char *, unsigned long, struct dentry **);
	void (*umount)(const char *);
};

struct super_block {
	struct list_head s_list;
	refcnt_t s_rc;
	dev_t s_dev;
	uint64_t s_magic;
	uint64_t s_block_size;
	unsigned long s_mount_flags;
	struct dentry *s_root;
	void *s_private;
	const struct file_system_type *s_fs_type;
	const struct super_operations *s_ops;
};

struct super_operations {
	void (*deinit_inode)(struct inode *);
	int (*read_inode)(struct inode *, void *);
};

struct inode {
	struct list_head i_list;
	refcnt_t i_rc;
	sleeplock_t i_lock;
	struct super_block *i_sb;
	uint32_t i_no;
	dev_t i_rdev;
	uint32_t i_flags;
	mode_t i_mode;
	uint32_t i_nlink;
	uint64_t i_size;
	uid_t i_uid;
	gid_t i_gid;
	struct timespec i_atime;
	struct timespec i_mtime;
	struct timespec i_ctime;
	void *i_private;
	const struct inode_operations *i_ops;
	const struct file_operations *i_fops;
};

struct inode_operations {
	int (*create)(struct dentry *, struct dentry *, mode_t);
	int (*link)(struct dentry *, struct dentry *, struct dentry *);
	int (*unlink)(struct dentry *, struct dentry *);
	int (*mkdir)(struct dentry *, struct dentry *, mode_t);
	int (*rmdir)(struct dentry *, struct dentry *);
	int (*lookup)(struct dentry *, struct dentry *);
	int (*mknod)(struct dentry *, struct dentry *, mode_t, dev_t);
	int (*rename)(struct dentry *, struct dentry *, struct dentry *,
		      struct dentry *);
	int (*symlink)(struct dentry *, struct dentry *, const char *);
	int (*readlink)(struct dentry *, char *, size_t);
};

#define FMODE_READ 1
#define FMODE_WRITE 2
#define FMODE_DIR 4

struct file {
	struct list_head f_list;
	refcnt_t f_rc;
	fmode_t f_mode;
	int f_flags;
	off_t f_off;
	struct inode *f_inode;
	struct pipe *f_pipe;
	struct dentry *f_dentry;
	const struct file_operations *f_ops;
};

struct file_operations {
	int (*open)(struct file *, struct dentry *, int);
	int (*close)(struct file *);
	int (*read)(struct file *, void *, size_t, off_t *, size_t *);
	int (*write)(struct file *, const void *, size_t, off_t *, size_t *);
	int (*stat)(struct file *, struct stat *);
	off_t (*seek)(struct file *, off_t, int);
	int (*truncate)(struct file *, off_t);
};

#define SEEK_SET 0 /* Seek from beginning of file.  */
#define SEEK_CUR 1 /* Seek from current position.  */
#define SEEK_END 2 /* Seek from end of file.  */

struct super_block *sblock_dup(struct super_block *sb);
void sblock_put(struct super_block *sb);
struct super_block *sblock_alloc(void);
void sblock_free(struct super_block *sb);
int sblock_add(struct super_block *sb);
refcnt_t sblock_rc(struct super_block *sb);

#define NR_ITABLE_BUCKETS 64

int inode_cache_init(void);
struct inode *inode_get(struct super_block *sb, uint32_t ino);
struct inode *inode_dup(struct inode *ip);
void inode_put(struct inode *ip);
struct inode *inode_alloc(void);
void inode_free(struct inode *ip);
int inode_add(struct inode *ip);
mode_t inode_mode(struct inode *ip);
refcnt_t inode_rc(struct inode *ip);

int file_cache_init(void);
struct file *file_alloc(void);
void file_put(struct file *fp);
struct file *file_dup(struct file *fp);
int file_read(struct file *fp, void *buf, size_t cnt, size_t *rcnt);
int file_write(struct file *fp, const void *buf, size_t cnt, size_t *wcnt);
off_t file_seek(struct file *fp, off_t offset, int whence);
int file_stat(struct file *fp, struct stat *buf);
int file_truncate(struct file *fp, off_t len);

extern const struct file_system_type ext4_fs_type;
extern const struct dentry_operations ext4_dops;
extern const struct super_operations ext4_sops;
extern const struct inode_operations ext4_iops;
extern const struct file_operations ext4_fops;

extern const struct file_system_type tmpfs_fs_type;
extern const struct dentry_operations tmpfs_dops;
extern const struct super_operations tmpfs_sops;
extern const struct inode_operations tmpfs_iops;
extern const struct file_operations tmpfs_fops;

extern const struct file_operations chrdev_fops;
extern const struct file_operations blkdev_fops;

extern const struct file_operations pipe_fops;

void fs_init(void);
int do_openat(int dirfd, const char *path, int flags, mode_t mode,
	      struct file **pfp);
int do_mkdirat(int dirfd, const char *path, mode_t mode);
int do_mknodat(int dirfd, const char *path, mode_t mode, dev_t dev);
int do_linkat(int olddirfd, const char *oldpath, int newdirfd,
	      const char *newpath, int flags);
int do_unlinkat(int dirfd, const char *path, int flags);
int do_execve(char *path, char **argv, char **envp);

#endif
