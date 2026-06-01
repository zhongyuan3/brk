#include <brk/dcache.h>
#include <brk/device.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/string.h>
#include <brk/tty.h>
#include <brk/virtio_blk.h>
#include <uapi/brk/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

static void register_builtin_filesystems(void)
{
	fs_driver_register(&tmpfs_fs_type);
	fs_driver_register(&brkfs_fs_type);
	fs_driver_register(&procfs_fs_type);
	pipe_fs_init();
}

int fs_init(void)
{
	struct process *proc;
	struct fs_mount_state *mnt;
	struct fs_mount_state *new_root_mnt;
	struct fs_path new_root_path;
	int err;

	register_builtin_filesystems();

	mnt = kernel_mount(&tmpfs_fs_type, 0, NULL, NULL);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	proc = current_process();
	struct fs_path root_path = {
		.mnt = mnt,
		.dentry = fs_dentry_get(mnt->root),
	};
	fsinfo_set_root(proc->fsinfo, &root_path);
	fs_path_get(&root_path);
	fsinfo_set_cwd(proc->fsinfo, &root_path);
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

	fsinfo_update_root(proc->fsinfo, &new_root_path);
	fsinfo_update_cwd(proc->fsinfo, &new_root_path);
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

	fdtable_alloc_fd(proc->fdtable, f);
	fdtable_dup_fd(proc->fdtable, 0);
	fdtable_dup_fd(proc->fdtable, 0);

	return 0;
}
