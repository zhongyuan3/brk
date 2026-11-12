#include <brk/devtmpfs.h>
#include <brk/fs.h>

static void register_builtin_filesystems(void)
{
	fs_driver_register(&tmpfs_fs_type);
	fs_driver_register(&devtmpfs_fs_type);
	fs_driver_register(&brkfs_fs_type);
	fs_driver_register(&procfs_fs_type);
	pipe_fs_init();
}

int fs_init(void)
{
	register_builtin_filesystems();
	return 0;
}
