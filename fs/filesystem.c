#include <aosd/fs.h>
#include <aosd/macros.h>
#include <aosd/string.h>

static struct file_system_type *file_systems[5] = {
	&ext4_fs_type,
	&tmpfs_fs_type,
};

struct file_system_type *get_fs_type(const char *fs_name)
{
	for (size_t i = 0; i < countof(file_systems); ++i)
		if (!strcmp(fs_name, file_systems[i]->name))
			return file_systems[i];

	return NULL;
}
