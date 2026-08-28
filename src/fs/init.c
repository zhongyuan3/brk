#include <brk/base/error.h>
#include <brk/drivers/tty.h>
#include <brk/drivers/virtio_blk.h>
#include <brk/fs/dcache.h>
#include <brk/fs/fdtable.h>
#include <brk/fs/fs.h>
#include <brk/fs/fsinfo.h>
#include <brk/fs/mount.h>
#include <brk/fs/path.h>
#include <brk/lib/string.h>
#include <brk/printk/printk.h>
#include <brk/process/task.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/fcntl.h>
#include <uapi/brk/stat.h>

static void register_builtin_filesystems(void)
{
	fs_driver_register(&tmpfs_fs_type);
	fs_driver_register(&brkfs_fs_type);
	fs_driver_register(&procfs_fs_type);
	pipe_fs_init();
}

int fs_init(void)
{
	struct task_control_block *task;
	struct fs_mount_state *mnt;
	struct fs_mount_state *new_root_mnt;
	struct fs_path new_root_path;
	int err;

	register_builtin_filesystems();

	mnt = kernel_mount(&tmpfs_fs_type, 0, NULL, NULL);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	task = current_task();
	struct fs_path root_path = {
		.mnt = mnt,
		.dentry = fs_dentry_get(mnt->root),
	};
	fsinfo_set_root(task->fsinfo, &root_path);
	fs_path_get(&root_path);
	fsinfo_set_cwd(task->fsinfo, &root_path);
	klog_info("root mount point initialized\n");

	err = do_mkdirat(AT_FDCWD, "/dev", 0755);
	if (err)
		return err;
	klog_info("/dev directory created\n");

	err = virtio_blk_mknod();
	if (err)
		return err;

	err = tty_mknod();
	if (err)
		return err;

	err = do_mount("/dev/virtio_blk0", "/", "brkfs", 0, NULL);
	if (err)
		return err;
	klog_info("/ mounted successfully\n");

	new_root_mnt = fs_mount_state_lookup(&root_path);
	if (!new_root_mnt)
		return -EINVAL;

	new_root_path.mnt = new_root_mnt;
	new_root_path.dentry = fs_dentry_get(new_root_mnt->root);
	fs_path_get(&new_root_path);

	fsinfo_update_root(task->fsinfo, &new_root_path);
	fsinfo_update_cwd(task->fsinfo, &new_root_path);
	fs_path_put(&root_path);
	fs_path_put(&new_root_path);

	err = do_mkdirat(AT_FDCWD, "/dev", 0755);
	if (err && err != -EEXIST)
		return err;
	if (err == -EEXIST)
		err = 0;

	err = do_mount(NULL, "/dev", "tmpfs", 0, NULL);
	if (err)
		return err;
	klog_info("/dev mounted successfully\n");

	err = do_mkdirat(AT_FDCWD, "/proc", 0755);
	if (err && err != -EEXIST)
		return err;
	if (err == -EEXIST)
		err = 0;

	err = do_mount(NULL, "/proc", "procfs", 0, NULL);
	if (err)
		return err;
	klog_info("/proc mounted successfully\n");

	err = tty_mknod();
	if (err)
		return err;

	struct fs_file *f = do_openat(AT_FDCWD, "/dev/tty0", O_RDWR, 0);
	if (IS_ERR(f)) {
		err = PTR_ERR(f);
		return err;
	}

	fdtable_alloc_fd(task->fdtable, f);
	fdtable_dup_fd(task->fdtable, 0);
	fdtable_dup_fd(task->fdtable, 0);

	return 0;
}
