#include <brk/devtmpfs.h>
#include <brk/fs.h>

static void register_builtin_filesystems(void)
{
	file_system_type_register(&tmpfs_fs_type);
	file_system_type_register(&devtmpfs_fs_type);
	file_system_type_register(&brkfs_fs_type);
	file_system_type_register(&procfs_fs_type);
	pipe_fs_init();
}

int vfs_init(void)
{
	register_builtin_filesystems();
	return 0;
}
