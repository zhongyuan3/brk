#include <brk/dcache.h>
#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fcntl.h>
#include <brk/fs.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/stat.h>
#include <brk/string.h>

static void register_builtin_filesystems(void)
{
	register_filesystem(&tmpfs_fs_type);
	register_filesystem(&brkfs_fs_type);
	pipe_fs_init();
}

int fs_init(void)
{
	register_builtin_filesystems();

	struct process *proc = current_process();

	int err = init_mount_tree(&proc->root);
	if (err)
		return err;

	path_dup(&proc->root);
	proc->cwd = proc->root;

	err = do_mknodat(AT_FDCWD, "/disk0", S_IFBLK, DEV_DISK0);
	if (err)
		return err;

	err = do_mount("/disk0", "/", "brkfs", 0, NULL);
	if (err)
		return err;

	struct mount *root_mnt = lookup_mount(&proc->root);
	if (!root_mnt)
		return -EINVAL;

	struct path root_path = {
		.mnt = root_mnt,
		.dentry = dentry_dup(root_mnt->mnt_root),
	};

	path_put(&proc->root);
	proc->root = root_path;
	path_put(&proc->cwd);
	path_dup(&root_path);
	proc->cwd = root_path;

	err = do_mkdirat(AT_FDCWD, "/dev", 0);
	if (err && err != -EEXIST)
		return err;
	if (err == -EEXIST)
		err = 0;

	err = do_mount(NULL, "/dev", "tmpfs", 0, NULL);
	if (err)
		return err;

	err = do_mknodat(AT_FDCWD, "/dev/console", S_IFCHR, DEV_CONSOLE0);
	if (err)
		return err;

	struct file *f = do_openat(AT_FDCWD, "/dev/console", O_RDWR, 0);
	if (IS_ERR(f)) {
		err = PTR_ERR(f);
		return err;
	}

	proc->ofiles[0] = f;
	proc->ofiles[1] = file_dup(f);
	proc->ofiles[2] = file_dup(f);

	return 0;
}
