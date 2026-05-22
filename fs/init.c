#include <brk/dcache.h>
#include <brk/device.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/string.h>
#include <brk/tty.h>
#include <brk/virtio_disk.h>
#include <uapi/brk/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

static void register_builtin_filesystems(void)
{
	register_filesystem(&tmpfs_fs_type);
	register_filesystem(&brkfs_fs_type);
	register_filesystem(&procfs_fs_type);
	pipe_fs_init();
}

int fs_init(void)
{
	register_builtin_filesystems();

	struct process *proc = current_process();

	int err = init_mount_tree(&proc->root);
	if (err)
		return err;
	klog_info("mount tree initialized\n");

	path_dup(&proc->root);
	proc->cwd = proc->root;

	err = do_mkdirat(AT_FDCWD, "/dev", 0755);
	if (err)
		return err;
	klog_info("/dev directory created\n");

	err = virtio_disk_create_fs_nodes();
	if (err)
		return err;
	klog_info("virtio disk fs nodes created\n");

	err = tty_create_fs_nodes();
	if (err)
		return err;
	klog_info("tty fs nodes created\n");

	err = do_mount("/dev/vdisk0", "/", "brkfs", 0, NULL);
	if (err)
		return err;
	klog_info("/ mounted successfully\n");

	struct fs_mount_state *root_mnt = lookup_mount(&proc->root);
	if (!root_mnt)
		return -EINVAL;

	struct file_anchor root_path = {
		.mnt = root_mnt,
		.dentry = dentry_dup(root_mnt->mnt_root),
	};

	path_put(&proc->root);
	proc->root = root_path;
	path_put(&proc->cwd);
	path_dup(&root_path);
	proc->cwd = root_path;

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

	err = virtio_disk_create_fs_nodes();
	if (err)
		return err;
	klog_info("virtio disk fs nodes created\n");

	err = tty_create_fs_nodes();
	if (err)
		return err;
	klog_info("tty fs nodes created\n");

	struct opened_file *f = do_openat(AT_FDCWD, "/dev/tty0", O_RDWR, 0);
	if (IS_ERR(f)) {
		err = PTR_ERR(f);
		return err;
	}
	klog_info("/dev/tty0 opened successfully\n");

	proc->ofiles[0] = f;
	proc->ofiles[1] = file_dup(f);
	proc->ofiles[2] = file_dup(f);

	return 0;
}
