#include <brk/devtmpfs.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/genhd.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/printf.h>
#include <brk/task.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

int rootfs_init(void)
{
	struct task *task;
	struct fs_mount_state *mnt;
	struct fs_mount_state *new_root_mnt;
	struct fs_path new_root_path;
	int err;

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

	err = do_mkdirat(AT_FDCWD, "/dev", 0755);
	if (err)
		return err;

	/* The brkfs mount below needs the block device nodes to exist already. */
	err = devtmpfs_populate();
	if (err)
		return err;

	/*
	 * Prefer the first brkfs primary partition; fall back to a filesystem
	 * written directly at the start of the whole disk.
	 */
	char root_dev[32];
	int mount_err = -ENODEV;

	for (unsigned int i = 1; i <= GENHD_MAX_PARTITIONS; ++i) {
		snprintf(root_dev, sizeof(root_dev), "/dev/virtio_blk0p%u", i);
		mount_err = do_mount(root_dev, "/", "brkfs", 0, NULL);
		if (!mount_err)
			break;
	}
	if (mount_err)
		mount_err = do_mount("/dev/virtio_blk0", "/", "brkfs", 0, NULL);
	err = mount_err;
	if (err)
		return err;

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

	err = do_mount(NULL, "/dev", "devtmpfs", 0, NULL);
	if (err)
		return err;

	err = do_mkdirat(AT_FDCWD, "/proc", 0755);
	if (err && err != -EEXIST)
		return err;
	if (err == -EEXIST)
		err = 0;

	err = do_mount(NULL, "/proc", "procfs", 0, NULL);
	if (err)
		return err;

	/* The devtmpfs instance is fresh, so recreate all device nodes. */
	err = devtmpfs_populate();
	if (err)
		return err;

	err = devtmpfs_start();
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
