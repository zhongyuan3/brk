#include <aosd/assert.h>
#include <aosd/dcache.h>
#include <aosd/dev.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/macros.h>
#include <aosd/mount.h>
#include <aosd/panic.h>
#include <aosd/path.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/process_types.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/types.h>
#include <uapi/aosd/fcntl.h>
#include <uapi/aosd/stat.h>

static LIST_DEFINE(mnt_list);
static SPINLOCK_DEFINE(mnt_list_lock);

struct vfsmount *mount_alloc(const char *mount_point)
{
	return kzalloc(sizeof(struct vfsmount));
}

void mount_free(struct vfsmount *mount)
{
	kfree(mount);
}

void mount_add(struct vfsmount *mount)
{
	spinlock_acquire(&mnt_list_lock);
	list_add(&mount->mnt_list, &mnt_list);
	spinlock_release(&mnt_list_lock);
}

static int __do_mount(struct file_system_type *fs_type, const char *dev_name,
		      const char *mount_point, int flags, struct dentry *parent)
{
	struct vfsmount *mnt = mount_alloc(mount_point);
	if (!mnt)
		return -ENOMEM;

	struct dentry *mnt_root = NULL;
	int err = fs_type->mount(fs_type, dev_name, mount_point, flags,
				 &mnt_root);
	if (err) {
		mount_free(mnt);
		return err;
	}

	mnt_root->d_parent = parent;
	dentry_add(mnt_root);

	mnt->mnt_sb = sblock_dup(mnt_root->d_inode->i_sb);
	mnt->mnt_flags = flags;
	mount_add(mnt);

	assert(sblock_rc(mnt->mnt_sb) == 2);
	assert(dentry_rc(mnt->mnt_sb->s_root) == 1);
	assert(inode_rc(mnt->mnt_sb->s_root->d_inode) == 1);

	return 0;
}

int do_mount(const char *fs_name, const char *dev_name, const char *mount_point,
	     int flags)
{
	struct file_system_type *fs_type = get_fs_type(fs_name);
	if (!fs_type)
		return -EINVAL;

	struct dentry *mnt_root = path_lookup(mount_point);
	if (!mnt_root)
		return -ENOENT;

	if (dentry_rc(mnt_root) > 1 ||
	    (dentry_flags(mnt_root) & DENTRY_MOUNTED)) {
		dentry_put(mnt_root);
		return -EBUSY;
	}
	struct dentry *mnt_parent = dentry_dup(mnt_root->d_parent);
	dentry_put(mnt_root);
	mnt_root = NULL;

	int err = __do_mount(fs_type, dev_name, mount_point, flags, mnt_parent);
	if (err) {
		dentry_put(mnt_parent);
		return err;
	}
	return 0;
}

int do_umount(const char *mount_point, int flags)
{
	struct dentry *mnt_root = path_lookup(mount_point);
	if (!mnt_root)
		return -ENOENT;

	if (!(dentry_flags(mnt_root) & DENTRY_MOUNTED)) {
		dentry_put(mnt_root);
		return -EINVAL;
	}

	if (dentry_rc(mnt_root) > 2) {
		dentry_put(mnt_root);
		return -EBUSY;
	}
	assert(dentry_rc(mnt_root) == 2);
	dentry_put(mnt_root);

	struct vfsmount *mnt;
	struct file_system_type *fs_type;

	spinlock_acquire(&mnt_list_lock);
	list_for_each_entry(mnt, &mnt_list, mnt_list) {
		if (mnt->mnt_sb->s_root == mnt_root) {
			if (sblock_rc(mnt->mnt_sb) > 2) {
				spinlock_release(&mnt_list_lock);
				return -EBUSY;
			}
			list_del(&mnt->mnt_list);
			spinlock_release(&mnt_list_lock);
			fs_type = mnt->mnt_sb->s_fs_type;
			fs_type->umount(mount_point);
			sblock_put(mnt->mnt_sb);
			mount_free(mnt);
			return 0;
		}
	}
	spinlock_release(&mnt_list_lock);

	return -EINVAL;
}

void fs_init(void)
{
	int err;

	err = __do_mount(&tmpfs_fs_type, NULL, "/", 0, NULL);
	assert(!err);

	err = do_mknodat(AT_FDCWD, "/disk0", S_IFBLK, DEV_DISK0);
	assert(!err);

	struct dentry *mnt_root = NULL;
	err = ext4_fs_type.mount(&ext4_fs_type, "/disk0", "/", 0, &mnt_root);
	assert(!err);

	err = do_umount("/", 0);
	assert(!err);

	mnt_root->d_parent = NULL;
	dentry_add(mnt_root);

	struct vfsmount *mnt = mount_alloc("/");
	assert(mnt);
	mnt->mnt_sb = sblock_dup(mnt_root->d_inode->i_sb);
	mnt->mnt_flags = 0;
	mount_add(mnt);

	assert(sblock_rc(mnt->mnt_sb) == 2);
	assert(dentry_rc(mnt->mnt_sb->s_root) == 1);
	assert(inode_rc(mnt->mnt_sb->s_root->d_inode) == 1);

	printk("fs_init() succeed\n");

	err = do_mkdirat(AT_FDCWD, "/dev", 0);
	assert(!err);

	err = do_mount("tmpfs", NULL, "/dev", 0);
	assert(!err);

	err = do_mknodat(AT_FDCWD, "/dev/console", S_IFCHR, DEV_CONSOLE0);
	assert(!err);

	struct file *f = NULL;
	err = do_openat(AT_FDCWD, "/dev/console", O_RDWR, 0, &f);
	assert(!err);

	struct process *proc = proc_get_current();
	proc->ofiles[0] = f;
	proc->ofiles[1] = file_dup(f);
	proc->ofiles[2] = file_dup(f);

	proc->cwd = path_lookup("/");
	assert(proc->cwd != NULL);
}
