#ifndef AOSD_MOUNT_H
#define AOSD_MOUNT_H

#include <aosd/types.h>

struct dentry;
struct super_block;
struct file_system_type;

struct vfsmount {
	struct super_block *mnt_sb;
	struct list_head mnt_list;
	int mnt_flags;
};

struct vfsmount *mount_alloc(const char *mount_point);
void mount_free(struct vfsmount *mount);
void mount_add(struct vfsmount *mount);

const struct file_system_type *get_fs_type(const char *fs_name);

int do_mount(const char *fs_name, const char *dev_name, const char *mount_point,
	     int flags);
int do_umount(const char *mount_point, int flags);

#endif
