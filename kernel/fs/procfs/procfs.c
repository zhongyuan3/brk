/*
 * procfs - lightweight pseudo filesystem exposing kernel state.
 *
 * The on-disk image is purely synthetic: every inode is generated on
 * demand from a key encoded in i_ino, and every regular file's body is
 * formatted lazily into a per-open snapshot buffer (the seq_file pattern,
 * stripped down to a single page).
 *
 * Layout
 * ------
 *   /proc/
 *     version          kernel build banner
 *     uptime           seconds since boot, idle (placeholder)
 *     meminfo          buddy allocator stats per order
 *     pagecache        page cache LRU size (debug)
 *     pagecache_shrink write N to manually reclaim N clean pages (debug)
 *     filesystems      one registered fs name per line
 *     <pid>/
 *       status         multi-line key:value process state
 *       stat           single-line ps-style summary
 *       cmdline        process name terminated by NUL
 *
 * Inode encoding
 * --------------
 *   The top 4 bits of i_ino classify the entry; the rest carry the
 *   target pid / static-entry index. This keeps every inode lookup
 *   stateless: from an inode we can rederive what it represents without
 *   ever allocating a side struct.
 *
 *     ROOT        ino = 1
 *     STATIC      ino = (1 << 60) | idx          1 <= idx <= n_static
 *     PID_DIR     ino = (2 << 60) | pid
 *     PID_FILE    ino = (3 << 60) | (pid << 8) | idx
 *
 * Lifecycle / locking
 * -------------------
 * The filesystem is read-only for normal entries (no create / unlink).
 * A few debug control files (e.g. pagecache_shrink) accept writes.
 * Snapshots taken at open() are immutable for the lifetime of the file;
 * concurrent kernel state changes do not perturb in-flight readers.
 *
 * Per-pid entries never carry any filesystem-private state, so eviction
 * is a no-op: when the process exits the next /proc/<pid> lookup fails
 * with ENOENT and the corresponding cached inode is dropped naturally
 * once dcache prunes the dentry.
 */

#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/pagecache.h>
#include <brk/path.h>
#include <brk/pgalloc.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>
#include <uapi/dirent.h>
#include <uapi/stat.h>

#define PROCFS_MAGIC 0x9fa0
#define PROCFS_BUF_SIZE PAGE_SIZE
#define PROCFS_PID_SNAPSHOT_MAX 256

#define PROC_INO_ROOT 1ULL
#define PROC_INO_KIND_SHIFT 60
#define PROC_INO_KIND_MASK 0xfULL
#define PROC_INO_IDX_MASK 0xffULL
#define PROC_INO_PID_MASK ((1ULL << PROC_INO_KIND_SHIFT) - 1)
#define PROC_INO_PID_FILE_PID_SHIFT 8

enum proc_kind {
	PROC_KIND_ROOT = 0,
	PROC_KIND_STATIC = 1,
	PROC_KIND_PID_DIR = 2,
	PROC_KIND_PID_FILE = 3,
};

struct procfs_entry;

typedef int (*procfs_show_fn)(const struct procfs_entry *entry, pid_t pid,
			      char *buf, size_t size);

typedef ssize_t (*procfs_write_fn)(const struct procfs_entry *entry,
				   const char *buf, size_t size, loff_t *pos);

struct procfs_entry {
	const char *name;
	u8 name_len;
	u8 idx; /* index used in the inode encoding (>= 1) */
	u8 d_type;
	umode_t mode;
	procfs_show_fn show;
	procfs_write_fn write;
};

static int procfs_show_version(const struct procfs_entry *e, pid_t pid,
			       char *buf, size_t size);
static int procfs_show_uptime(const struct procfs_entry *e, pid_t pid,
			      char *buf, size_t size);
static int procfs_show_meminfo(const struct procfs_entry *e, pid_t pid,
			       char *buf, size_t size);
static int procfs_show_filesystems(const struct procfs_entry *e, pid_t pid,
				   char *buf, size_t size);
static int procfs_show_pagecache(const struct procfs_entry *e, pid_t pid,
				 char *buf, size_t size);
static int procfs_show_pagecache_shrink(const struct procfs_entry *e, pid_t pid,
					char *buf, size_t size);
static ssize_t procfs_write_pagecache_shrink(const struct procfs_entry *e,
					     const char *buf, size_t size,
					     loff_t *pos);
static int procfs_show_pid_status(const struct procfs_entry *e, pid_t pid,
				  char *buf, size_t size);
static int procfs_show_pid_stat(const struct procfs_entry *e, pid_t pid,
				char *buf, size_t size);
static int procfs_show_pid_cmdline(const struct procfs_entry *e, pid_t pid,
				   char *buf, size_t size);

static const struct procfs_entry procfs_root_entries[] = {
	{ .name = "version",
	  .name_len = 7,
	  .idx = 1,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0444,
	  .show = procfs_show_version },
	{ .name = "uptime",
	  .name_len = 6,
	  .idx = 2,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0444,
	  .show = procfs_show_uptime },
	{ .name = "meminfo",
	  .name_len = 7,
	  .idx = 3,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0444,
	  .show = procfs_show_meminfo },
	{ .name = "filesystems",
	  .name_len = 11,
	  .idx = 4,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0444,
	  .show = procfs_show_filesystems },
	{ .name = "pagecache",
	  .name_len = 9,
	  .idx = 5,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0444,
	  .show = procfs_show_pagecache },
	{ .name = "pagecache_shrink",
	  .name_len = 16,
	  .idx = 6,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0644,
	  .show = procfs_show_pagecache_shrink,
	  .write = procfs_write_pagecache_shrink },
};
#define PROCFS_ROOT_ENTRIES_NR countof(procfs_root_entries)

static const struct procfs_entry procfs_pid_entries[] = {
	{ .name = "status",
	  .name_len = 6,
	  .idx = 1,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0444,
	  .show = procfs_show_pid_status },
	{ .name = "stat",
	  .name_len = 4,
	  .idx = 2,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0444,
	  .show = procfs_show_pid_stat },
	{ .name = "cmdline",
	  .name_len = 7,
	  .idx = 3,
	  .d_type = DT_REG,
	  .mode = S_IFREG | 0444,
	  .show = procfs_show_pid_cmdline },
};
#define PROCFS_PID_ENTRIES_NR countof(procfs_pid_entries)

static inline enum proc_kind procfs_ino_kind(unsigned long ino)
{
	if (ino == PROC_INO_ROOT)
		return PROC_KIND_ROOT;
	return (ino >> PROC_INO_KIND_SHIFT) & PROC_INO_KIND_MASK;
}

static inline int procfs_ino_idx(unsigned long ino)
{
	return ino & PROC_INO_IDX_MASK;
}

static inline pid_t procfs_ino_pid(unsigned long ino)
{
	switch (procfs_ino_kind(ino)) {
	case PROC_KIND_PID_DIR:
		return ino & PROC_INO_PID_MASK;
	case PROC_KIND_PID_FILE:
		return (ino >> PROC_INO_PID_FILE_PID_SHIFT) &
		       (PROC_INO_PID_MASK >> PROC_INO_PID_FILE_PID_SHIFT);
	default:
		return 0;
	}
}

static inline unsigned long procfs_make_static_ino(int idx)
{
	return ((unsigned long)PROC_KIND_STATIC << PROC_INO_KIND_SHIFT) |
	       (unsigned long)idx;
}

static inline unsigned long procfs_make_pid_dir_ino(pid_t pid)
{
	return ((unsigned long)PROC_KIND_PID_DIR << PROC_INO_KIND_SHIFT) |
	       (unsigned long)pid;
}

static inline unsigned long procfs_make_pid_file_ino(pid_t pid, int idx)
{
	return ((unsigned long)PROC_KIND_PID_FILE << PROC_INO_KIND_SHIFT) |
	       ((unsigned long)pid << PROC_INO_PID_FILE_PID_SHIFT) |
	       (unsigned long)idx;
}

static const struct procfs_entry *procfs_static_by_idx(int idx)
{
	for (size_t i = 0; i < PROCFS_ROOT_ENTRIES_NR; ++i)
		if (procfs_root_entries[i].idx == idx)
			return &procfs_root_entries[i];
	return NULL;
}

static const struct procfs_entry *procfs_pid_by_idx(int idx)
{
	for (size_t i = 0; i < PROCFS_PID_ENTRIES_NR; ++i)
		if (procfs_pid_entries[i].idx == idx)
			return &procfs_pid_entries[i];
	return NULL;
}

static const char *procfs_state_name(enum task_state state)
{
	switch (state) {
	case TASK_STATE_NEW:
		return "new";
	case TASK_STATE_RUNNING:
		return "running";
	case TASK_STATE_SLEEPING:
		return "sleeping";
	case TASK_STATE_ZOMBIE:
		return "zombie";
	}
	return "unknown";
}

static char procfs_state_letter(enum task_state state)
{
	switch (state) {
	case TASK_STATE_NEW:
		return 'N';
	case TASK_STATE_RUNNING:
		return 'R';
	case TASK_STATE_SLEEPING:
		return 'S';
	case TASK_STATE_ZOMBIE:
		return 'Z';
	}
	return '?';
}

static int procfs_show_version(const struct procfs_entry *e, pid_t pid,
			       char *buf, size_t size)
{
	(void)e;
	(void)pid;
	return snprintf(buf, size,
			"BRK kernel " __DATE__ " " __TIME__ " riscv64\n");
}

static int procfs_show_uptime(const struct procfs_entry *e, pid_t pid,
			      char *buf, size_t size)
{
	(void)e;
	(void)pid;
	struct timespec ts;

	ktime_get_boot_ts(&ts);

	/*
	 * Two fields, mirroring Linux's /proc/uptime: total wall time and
	 * cumulative idle time. We do not yet track idle time, so the
	 * second field is reported as 0.00.
	 */
	return snprintf(buf, size, "%lu.%02lu 0.00\n", (unsigned long)ts.tv_sec,
			(unsigned long)(ts.tv_nsec / 10000000UL));
}

struct procfs_meminfo_acc {
	char *buf;
	size_t size;
	size_t pos;
	int err;
};

static int procfs_show_meminfo(const struct procfs_entry *e, pid_t pid,
			       char *buf, size_t size)
{
	(void)e;
	(void)pid;

	/*
	 * We don't have a global "free pages" counter today; expose what
	 * the buddy allocator can easily produce: a per-order histogram by
	 * doing a probe-and-release pass. Since this can be racy and is
	 * also somewhat invasive (it perturbs allocator state), we keep it
	 * simple and just report the per-order maximum allocation order
	 * that currently succeeds. A real counter belongs in pgalloc.c;
	 * this is a placeholder that compiles today.
	 *
	 * For now expose only static information: page size and the buddy
	 * allocator's maximum order. Real free-page accounting can be
	 * added by extending pgalloc with stat counters.
	 */
	int n = snprintf(buf, size,
			 "PageSize:       %u kB\n"
			 "PageOrderMax:   %u\n",
			 (unsigned int)(PAGE_SIZE / 1024), PAGE_ORDER_MAX);
	return n;
}

struct procfs_fs_iter {
	char *buf;
	size_t size;
	size_t pos;
};

static void procfs_fs_collect(const struct fs_driver *fs, void *ctx)
{
	struct procfs_fs_iter *it = ctx;
	int n;

	if (it->pos >= it->size)
		return;
	n = snprintf(it->buf + it->pos, it->size - it->pos, "%s\n", fs->name);
	if (n > 0)
		it->pos += (size_t)n;
}

static int procfs_show_filesystems(const struct procfs_entry *e, pid_t pid,
				   char *buf, size_t size)
{
	(void)e;
	(void)pid;
	struct procfs_fs_iter it = { .buf = buf, .size = size, .pos = 0 };

	fs_driver_for_each(procfs_fs_collect, &it);
	return (int)it.pos;
}

static int procfs_show_pagecache(const struct procfs_entry *e, pid_t pid,
				 char *buf, size_t size)
{
	(void)e;
	(void)pid;

	return snprintf(buf, size,
			"nrpages:\t%lu\n"
			"reclaim:\twrite N to /proc/pagecache_shrink\n",
			page_cache_nr_pages());
}

static int procfs_show_pagecache_shrink(const struct procfs_entry *e, pid_t pid,
					char *buf, size_t size)
{
	(void)e;
	(void)pid;

	return snprintf(buf, size,
			"# write the number of clean pages to reclaim\n"
			"# example: echo 8 > /proc/pagecache_shrink\n");
}

static unsigned long procfs_parse_ulong(const char *buf, size_t size)
{
	unsigned long val = 0;
	bool seen = false;

	for (size_t i = 0; i < size; ++i) {
		char c = buf[i];

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;
		if (c < '0' || c > '9')
			break;
		seen = true;
		val = val * 10 + (unsigned long)(c - '0');
	}
	return seen ? val : 0;
}

static ssize_t procfs_write_pagecache_shrink(const struct procfs_entry *e,
					     const char *buf, size_t size,
					     loff_t *pos)
{
	unsigned long nr;
	unsigned long freed;

	(void)e;

	if (*pos != 0)
		return -EINVAL;
	if (size == 0)
		return -EINVAL;

	nr = procfs_parse_ulong(buf, size);
	if (nr == 0)
		return -EINVAL;

	freed = page_cache_shrink(nr);
	klog_info("pagecache: shrink(%lu) freed %lu pages (%lu remain)\n", nr,
		  freed, page_cache_nr_pages());
	*pos = (loff_t)size;
	return (ssize_t)size;
}

static int procfs_show_pid_status(const struct procfs_entry *e, pid_t pid,
				  char *buf, size_t size)
{
	(void)e;
	struct task_info info;

	if (!task_get_info(pid, &info))
		return -ESRCH;

	return snprintf(buf, size,
			"Name:\t%s\n"
			"State:\t%c (%s)\n"
			"Pid:\t%ld\n"
			"PPid:\t%ld\n"
			"Killed:\t%d\n"
			"Brk:\t0x%lx\n"
			"Utime:\t%lu\n"
			"Stime:\t%lu\n",
			info.name, procfs_state_letter(info.state),
			procfs_state_name(info.state), (long)info.pid,
			(long)info.ppid, info.killed ? 1 : 0,
			(unsigned long)info.brk, (unsigned long)info.utime,
			(unsigned long)info.ktime);
}

static int procfs_show_pid_stat(const struct procfs_entry *e, pid_t pid,
				char *buf, size_t size)
{
	(void)e;
	struct task_info info;

	if (!task_get_info(pid, &info))
		return -ESRCH;

	/*
	 * Mimic the leading fields of Linux's /proc/<pid>/stat so simple
	 * tools (ps, top) can parse us:
	 *   pid (comm) state ppid utime stime brk
	 */
	return snprintf(buf, size, "%ld (%s) %c %ld %lu %lu 0x%lx\n",
			(long)info.pid, info.name,
			procfs_state_letter(info.state), (long)info.ppid,
			(unsigned long)info.utime, (unsigned long)info.ktime,
			(unsigned long)info.brk);
}

static int procfs_show_pid_cmdline(const struct procfs_entry *e, pid_t pid,
				   char *buf, size_t size)
{
	(void)e;
	struct task_info info;
	size_t n;

	if (!task_get_info(pid, &info))
		return -ESRCH;

	n = strlen(info.name);
	if (n + 1 > size)
		n = size > 0 ? size - 1 : 0;
	if (n > 0)
		memcpy(buf, info.name, n);
	if (size > n)
		buf[n] = '\0';
	/* The trailing NUL is part of cmdline (one argv element). */
	return (int)(n + (size > n ? 1 : 0));
}

static const struct fs_file_ops procfs_root_dir_fops;
static const struct fs_file_ops procfs_pid_dir_fops;
static const struct fs_file_ops procfs_file_fops;
static const struct fs_inode_ops procfs_root_iops;
static const struct fs_inode_ops procfs_pid_dir_iops;
static const struct fs_inode_ops procfs_file_iops;

static void procfs_init_inode(struct fs_inode *inode, umode_t mode,
			      unsigned int nlink)
{
	inode->mode = mode;
	inode->nlink = nlink;
	inode->uid = 0;
	inode->gid = 0;
	inode->size = 0;
	inode->rdev = 0;
	inode_times_set_all_now(inode);
}

static struct fs_inode *procfs_iget(struct fs_super_block *sb,
				    unsigned long ino, umode_t mode,
				    unsigned int nlink,
				    const struct fs_inode_ops *iop,
				    const struct fs_file_ops *fop)
{
	struct fs_inode *inode = fs_inode_get_locked(sb, ino);
	if (!inode)
		return NULL;
	if (inode->state & I_NEW) {
		procfs_init_inode(inode, mode, nlink);
		inode->ops = iop;
		inode->fops = fop;
		fs_inode_unlock_new(inode);
	}
	return inode;
}

static struct fs_inode *procfs_iget_root(struct fs_super_block *sb)
{
	return procfs_iget(sb, PROC_INO_ROOT, S_IFDIR | 0555, 2,
			   &procfs_root_iops, &procfs_root_dir_fops);
}

static struct fs_inode *procfs_iget_static(struct fs_super_block *sb,
					   const struct procfs_entry *e)
{
	return procfs_iget(sb, procfs_make_static_ino(e->idx), e->mode, 1,
			   &procfs_file_iops, &procfs_file_fops);
}

static struct fs_inode *procfs_iget_pid_dir(struct fs_super_block *sb,
					    pid_t pid)
{
	return procfs_iget(sb, procfs_make_pid_dir_ino(pid), S_IFDIR | 0555, 2,
			   &procfs_pid_dir_iops, &procfs_pid_dir_fops);
}

static struct fs_inode *procfs_iget_pid_file(struct fs_super_block *sb,
					     pid_t pid,
					     const struct procfs_entry *e)
{
	return procfs_iget(sb, procfs_make_pid_file_ino(pid, e->idx), e->mode,
			   1, &procfs_file_iops, &procfs_file_fops);
}

static bool procfs_parse_pid(const char *name, int len, pid_t *out)
{
	pid_t v = 0;

	if (len <= 0)
		return false;
	for (int i = 0; i < len; ++i) {
		char c = name[i];
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + (c - '0');
		if (v > (pid_t)0xfffffff) /* keep ino encoding safe */
			return false;
	}
	*out = v;
	return true;
}

static void procfs_evict_inode(struct fs_inode *inode)
{
	(void)inode;
	/* Nothing per-inode is allocated, so there is nothing to free. */
}

static void procfs_put_super(struct fs_super_block *sb)
{
	struct fs_driver *driver = sb->driver;
	spinlock_acquire(&driver->lock);
	list_del(&sb->instance);
	spinlock_release(&driver->lock);
	if (sb->root)
		fs_dentry_put(sb->root);
}

static const struct fs_super_block_ops procfs_sops = {
	.evict_inode = procfs_evict_inode,
	.put_super = procfs_put_super,
};

static struct fs_dentry *procfs_root_lookup(struct fs_inode *dir,
					    struct fs_dentry *dentry,
					    unsigned int flags)
{
	(void)flags;
	struct fs_super_block *sb = dir->sb;
	const char *name = dentry->name.name;
	int len = dentry->name.len;
	pid_t pid;

	for (size_t i = 0; i < PROCFS_ROOT_ENTRIES_NR; ++i) {
		const struct procfs_entry *e = &procfs_root_entries[i];
		if (e->name_len == len && !memcmp(e->name, name, len)) {
			struct fs_inode *inode = procfs_iget_static(sb, e);
			if (!inode)
				return ERR_PTR(-ENOMEM);
			return fs_dentry_splice_alias(inode, dentry);
		}
	}

	if (procfs_parse_pid(name, len, &pid)) {
		if (!task_pid_exists(pid))
			return NULL;
		struct fs_inode *inode = procfs_iget_pid_dir(sb, pid);
		if (!inode)
			return ERR_PTR(-ENOMEM);
		return fs_dentry_splice_alias(inode, dentry);
	}

	return NULL;
}

static struct fs_dentry *procfs_pid_dir_lookup(struct fs_inode *dir,
					       struct fs_dentry *dentry,
					       unsigned int flags)
{
	(void)flags;
	struct fs_super_block *sb = dir->sb;
	pid_t pid = procfs_ino_pid(dir->ino);
	const char *name = dentry->name.name;
	int len = dentry->name.len;

	if (!task_pid_exists(pid))
		return ERR_PTR(-ENOENT);

	for (size_t i = 0; i < PROCFS_PID_ENTRIES_NR; ++i) {
		const struct procfs_entry *e = &procfs_pid_entries[i];
		if (e->name_len == len && !memcmp(e->name, name, len)) {
			struct fs_inode *inode =
				procfs_iget_pid_file(sb, pid, e);
			if (!inode)
				return ERR_PTR(-ENOMEM);
			return fs_dentry_splice_alias(inode, dentry);
		}
	}
	return NULL;
}

static int procfs_getattr(const struct fs_path *path, struct stat *stat,
			  u32 mask, unsigned int flags)
{
	(void)mask;
	(void)flags;
	struct fs_inode *inode = path->dentry->inode;

	memset(stat, 0, sizeof(*stat));
	stat->st_ino = inode->ino;
	stat->st_mode = inode->mode;
	stat->st_nlink = inode->nlink;
	stat->st_rdev = inode->rdev;
	stat->st_size = inode->size;
	stat->st_blksize = PAGE_SIZE;
	stat->st_blocks = 0;
	inode_times_to_stat(inode, stat);
	return 0;
}

static int procfs_setattr(struct fs_dentry *dentry, struct fs_iattr *attr)
{
	(void)dentry;
	(void)attr;
	return -EROFS;
}

static const struct fs_inode_ops procfs_root_iops = {
	.lookup = procfs_root_lookup,
	.getattr = procfs_getattr,
	.setattr = procfs_setattr,
};

static const struct fs_inode_ops procfs_pid_dir_iops = {
	.lookup = procfs_pid_dir_lookup,
	.getattr = procfs_getattr,
	.setattr = procfs_setattr,
};

static const struct fs_inode_ops procfs_file_iops = {
	.getattr = procfs_getattr,
	.setattr = procfs_setattr,
};

struct procfs_file_priv {
	const struct procfs_entry *entry;
	char *buf;
	size_t len; /* bytes actually generated */
	size_t cap; /* allocation size */
};

static int procfs_file_open(struct fs_inode *inode, struct fs_file *file)
{
	const struct procfs_entry *e;
	procfs_show_fn show;
	pid_t pid;
	int n;
	struct procfs_file_priv *priv;

	switch (procfs_ino_kind(inode->ino)) {
	case PROC_KIND_STATIC:
		e = procfs_static_by_idx(procfs_ino_idx(inode->ino));
		pid = 0;
		break;
	case PROC_KIND_PID_FILE:
		e = procfs_pid_by_idx(procfs_ino_idx(inode->ino));
		pid = procfs_ino_pid(inode->ino);
		break;
	default:
		return -EINVAL;
	}
	if (!e || !e->show)
		return -EINVAL;
	show = e->show;

	priv = kmalloc(sizeof(*priv));
	if (!priv)
		return -ENOMEM;
	priv->buf = kmalloc(PROCFS_BUF_SIZE);
	if (!priv->buf) {
		kfree(priv);
		return -ENOMEM;
	}
	priv->cap = PROCFS_BUF_SIZE;

	n = show(e, pid, priv->buf, priv->cap);
	if (n < 0) {
		kfree(priv->buf);
		kfree(priv);
		return n;
	}
	if ((size_t)n > priv->cap)
		n = (int)priv->cap;
	priv->len = (size_t)n;
	priv->entry = e;
	file->private_data = priv;
	return 0;
}

static int procfs_file_release(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;
	struct procfs_file_priv *priv = file->private_data;
	if (priv) {
		kfree(priv->buf);
		kfree(priv);
		file->private_data = NULL;
	}
	return 0;
}

static ssize_t procfs_file_read(struct fs_file *file, char *buf, size_t size,
				loff_t *pos)
{
	struct procfs_file_priv *priv = file->private_data;
	size_t remaining, n;

	if (!priv)
		return -EBADF;
	if (*pos < 0)
		return -EINVAL;
	if ((size_t)*pos >= priv->len)
		return 0;
	remaining = priv->len - (size_t)*pos;
	n = size < remaining ? size : remaining;
	memcpy(buf, priv->buf + (size_t)*pos, n);
	*pos += (loff_t)n;
	return (ssize_t)n;
}

static ssize_t procfs_file_write(struct fs_file *file, const char *buf,
				 size_t size, loff_t *pos)
{
	struct procfs_file_priv *priv = file->private_data;

	if (!priv || !priv->entry || !priv->entry->write)
		return -EROFS;
	return priv->entry->write(priv->entry, buf, size, pos);
}

static loff_t procfs_file_llseek(struct fs_file *file, loff_t offset,
				 int whence)
{
	struct procfs_file_priv *priv = file->private_data;
	loff_t end = priv ? (loff_t)priv->len : 0;
	loff_t new_pos;

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = file->pos + offset;
		break;
	case SEEK_END:
		new_pos = end + offset;
		break;
	default:
		return -EINVAL;
	}
	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int procfs_file_iterate_shared(struct fs_file *file,
				      struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -ENOTDIR;
}

static int procfs_file_fsync(struct fs_file *file, loff_t start, loff_t end,
			     int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int procfs_file_flush(struct fs_file *file)
{
	(void)file;
	return 0;
}

static long procfs_file_ioctl(struct fs_file *file, unsigned int cmd,
			      unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

static const struct fs_file_ops procfs_file_fops = {
	.open = procfs_file_open,
	.release = procfs_file_release,
	.read = procfs_file_read,
	.write = procfs_file_write,
	.llseek = procfs_file_llseek,
	.iterate_shared = procfs_file_iterate_shared,
	.fsync = procfs_file_fsync,
	.flush = procfs_file_flush,
	.ioctl = procfs_file_ioctl,
};

static int procfs_dir_open(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static int procfs_dir_release(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static ssize_t procfs_dir_read(struct fs_file *file, char *buf, size_t size,
			       loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static ssize_t procfs_dir_write(struct fs_file *file, const char *buf,
				size_t size, loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static loff_t procfs_dir_llseek(struct fs_file *file, loff_t offset, int whence)
{
	loff_t new_pos;

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = file->pos + offset;
		break;
	default:
		return -EINVAL;
	}
	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static unsigned long procfs_parent_ino(const struct fs_inode *inode)
{
	/*
	 * For the root inode there is no in-fs parent; reusing the inode's
	 * own ino is benign because crossing a mount point in '..' is
	 * handled by the VFS, not by the filesystem.
	 */
	if (inode->ino == PROC_INO_ROOT)
		return PROC_INO_ROOT;
	return PROC_INO_ROOT;
}

static bool procfs_emit_dot(struct fs_dir_iterator *ctx, struct fs_inode *dir)
{
	if (ctx->pos == 0) {
		if (!ctx->actor(ctx, ".", 1, ctx->pos, dir->ino, DT_DIR))
			return false;
		ctx->pos = 1;
	}
	if (ctx->pos == 1) {
		if (!ctx->actor(ctx, "..", 2, ctx->pos, procfs_parent_ino(dir),
				DT_DIR))
			return false;
		ctx->pos = 2;
	}
	return true;
}

static int procfs_root_iterate_shared(struct fs_file *file,
				      struct fs_dir_iterator *ctx)
{
	struct fs_inode *dir = file->inode;
	loff_t idx;
	pid_t *pids;
	int npids;

	if (!procfs_emit_dot(ctx, dir))
		return 0;

	for (size_t i = 0; i < PROCFS_ROOT_ENTRIES_NR; ++i) {
		idx = (loff_t)(2 + i);
		if (ctx->pos > idx)
			continue;
		const struct procfs_entry *e = &procfs_root_entries[i];
		unsigned long ino = procfs_make_static_ino(e->idx);
		if (!ctx->actor(ctx, e->name, e->name_len, ctx->pos, ino,
				e->d_type))
			return 0;
		ctx->pos = idx + 1;
	}

	/* Emit one entry per live process, ordered by snapshot index. */
	pids = kmalloc(sizeof(*pids) * PROCFS_PID_SNAPSHOT_MAX);
	if (!pids)
		return -ENOMEM;
	npids = task_snapshot_pids(pids, PROCFS_PID_SNAPSHOT_MAX);

	for (int i = 0; i < npids; ++i) {
		char name[16];
		int nlen;

		idx = (loff_t)(2 + PROCFS_ROOT_ENTRIES_NR + i);
		if (ctx->pos > idx)
			continue;

		nlen = snprintf(name, sizeof(name), "%ld", (long)pids[i]);
		if (nlen <= 0)
			continue;
		if ((size_t)nlen >= sizeof(name))
			nlen = (int)sizeof(name) - 1;
		unsigned long ino = procfs_make_pid_dir_ino(pids[i]);
		if (!ctx->actor(ctx, name, nlen, ctx->pos, ino, DT_DIR)) {
			kfree(pids);
			return 0;
		}
		ctx->pos = idx + 1;
	}

	kfree(pids);
	return 0;
}

static int procfs_pid_dir_iterate_shared(struct fs_file *file,
					 struct fs_dir_iterator *ctx)
{
	struct fs_inode *dir = file->inode;
	pid_t pid = procfs_ino_pid(dir->ino);
	loff_t idx;

	if (!task_pid_exists(pid))
		return -ENOENT;

	if (!procfs_emit_dot(ctx, dir))
		return 0;

	for (size_t i = 0; i < PROCFS_PID_ENTRIES_NR; ++i) {
		idx = (loff_t)(2 + i);
		if (ctx->pos > idx)
			continue;
		const struct procfs_entry *e = &procfs_pid_entries[i];
		unsigned long ino = procfs_make_pid_file_ino(pid, e->idx);
		if (!ctx->actor(ctx, e->name, e->name_len, ctx->pos, ino,
				e->d_type))
			return 0;
		ctx->pos = idx + 1;
	}

	return 0;
}

static int procfs_dir_fsync(struct fs_file *file, loff_t start, loff_t end,
			    int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int procfs_dir_flush(struct fs_file *file)
{
	(void)file;
	return 0;
}

static long procfs_dir_ioctl(struct fs_file *file, unsigned int cmd,
			     unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

static const struct fs_file_ops procfs_root_dir_fops = {
	.open = procfs_dir_open,
	.release = procfs_dir_release,
	.read = procfs_dir_read,
	.write = procfs_dir_write,
	.llseek = procfs_dir_llseek,
	.iterate_shared = procfs_root_iterate_shared,
	.fsync = procfs_dir_fsync,
	.flush = procfs_dir_flush,
	.ioctl = procfs_dir_ioctl,
};

static const struct fs_file_ops procfs_pid_dir_fops = {
	.open = procfs_dir_open,
	.release = procfs_dir_release,
	.read = procfs_dir_read,
	.write = procfs_dir_write,
	.llseek = procfs_dir_llseek,
	.iterate_shared = procfs_pid_dir_iterate_shared,
	.fsync = procfs_dir_fsync,
	.flush = procfs_dir_flush,
	.ioctl = procfs_dir_ioctl,
};

int procfs_mount(struct fs_mount_args *args, struct fs_mount_result *result)
{
	struct fs_super_block *sb;
	struct fs_inode *root_inode;
	struct fs_dentry *root_dentry;
	sb = fs_super_block_alloc(args->driver);
	if (!sb)
		return -ENOMEM;

	sb->block_size = PAGE_SIZE;
	sb->magic = PROCFS_MAGIC;
	sb->flags = args->flags;
	sb->ops = &procfs_sops;
	sb->default_dops = &generic_dop;
	sb->private_data = NULL;

	root_inode = procfs_iget_root(sb);
	if (!root_inode) {
		fs_super_block_free(sb);
		return -ENOMEM;
	}

	root_dentry = fs_dentry_make_root(root_inode);
	if (!root_dentry) {
		fs_inode_put(root_inode);
		fs_super_block_free(sb);
		return -ENOMEM;
	}

	sb->root = fs_dentry_get(root_dentry);

	spinlock_acquire(&args->driver->lock);
	list_add_tail(&sb->instance, &args->driver->super_blocks);
	spinlock_release(&args->driver->lock);

	result->root = root_dentry;
	result->sb = sb;

	return 0;
}

struct fs_driver procfs_fs_type = {
	.name = "procfs",
	.mount = procfs_mount,
	.lock = SPINLOCK_INITIALIZER("procfs_fs_lock"),
	.super_blocks = LIST_INITIALIZER(procfs_fs_type.super_blocks),
	.list = LIST_INITIALIZER(procfs_fs_type.list),
};
