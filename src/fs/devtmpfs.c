#include <brk/devtmpfs.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/printk.h>
#include <brk/printf.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

struct devtmpfs_work {
	struct list_head list;
	char name[DEVICE_NAME_LEN];
	dev_t dev;
	umode_t mode;
	bool remove;
};

static LIST_DEFINE(work_list);
static SPINLOCK_DEFINE(work_lock);
static bool active;

static int devtmpfs_create_node(const char *name, dev_t dev, umode_t mode)
{
	char path[DEVICE_NAME_LEN + 8];
	int err;

	snprintf(path, sizeof(path), "/dev/%s", name);
	err = do_mknodat(AT_FDCWD, path, mode, dev);
	if (err && err != -EEXIST)
		pr_warn("devtmpfs: create %s failed: %d\n", path, err);
	return err;
}

static void devtmpfs_remove_node(const char *name)
{
	char path[DEVICE_NAME_LEN + 8];
	int err;

	snprintf(path, sizeof(path), "/dev/%s", name);
	err = do_unlinkat(AT_FDCWD, path, 0);
	if (err && err != -ENOENT)
		pr_warn("devtmpfs: remove %s failed: %d\n", path, err);
}

static void devtmpfs_worker(void *arg)
{
	(void)arg;

	for (;;) {
		struct devtmpfs_work *work;
		irqstate_t flags;

		flags = spin_lock_irqsave(&work_lock);
		while (list_empty(&work_list))
			task_sleep(&work_list, &work_lock);

		work = list_first_entry(&work_list, struct devtmpfs_work,
					list);
		list_del_init(&work->list);
		spin_unlock_irqrestore(&work_lock, flags);

		if (work->remove)
			devtmpfs_remove_node(work->name);
		else
			devtmpfs_create_node(work->name, work->dev,
					     work->mode);

		kfree(work);
	}
}

void devtmpfs_notify(struct device *dev, bool remove)
{
	struct devtmpfs_work *work;
	irqstate_t flags;

	if (!dev || !dev->devt)
		return;

	work = kmalloc(sizeof(*work));
	if (!work)
		return;

	strlcpy(work->name, dev->name, sizeof(work->name));
	work->dev = dev->devt;
	work->mode = dev->mode;
	work->remove = remove;
	list_init(&work->list);

	flags = spin_lock_irqsave(&work_lock);
	if (!active) {
		/* Boot-time devices are handled by devtmpfs_populate(). */
		spin_unlock_irqrestore(&work_lock, flags);
		kfree(work);
		return;
	}
	list_add_tail(&work->list, &work_list);
	task_wake_all(&work_list);
	spin_unlock_irqrestore(&work_lock, flags);
}

int devtmpfs_populate(void)
{
	struct device **devices;
	size_t count;
	size_t i;
	int first_err = 0;

	devices = device_snapshot(&count);

	for (i = 0; i < count; ++i) {
		struct device *dev = devices[i];
		int err;

		if (!dev->devt)
			continue;

		err = devtmpfs_create_node(dev->name, dev->devt, dev->mode);
		if (err && err != -EEXIST && !first_err)
			first_err = err;
	}

	device_snapshot_free(devices);
	return first_err;
}

int devtmpfs_start(void)
{
	struct task *task;
	irqstate_t flags;

	flags = spin_lock_irqsave(&work_lock);
	if (active) {
		spin_unlock_irqrestore(&work_lock, flags);
		return 0;
	}
	active = true;
	spin_unlock_irqrestore(&work_lock, flags);

	task = kthread_create(devtmpfs_worker, NULL);
	if (!task) {
		flags = spin_lock_irqsave(&work_lock);
		active = false;
		spin_unlock_irqrestore(&work_lock, flags);
		return -ENOMEM;
	}

	/* Inherit root/cwd so absolute /dev paths resolve correctly. */
	fsinfo_copy(task->fsinfo, current_task()->fsinfo);
	strlcpy(task->name, "devtmpfs", sizeof(task->name));
	kthread_join(task);

	return 0;
}

static int devtmpfs_mount(struct fs_mount_args *args,
			  struct fs_mount_result *result)
{
	/*
	 * Storage and inode operations are shared with tmpfs.  devtmpfs adds
	 * automatic device node management on top.
	 */
	return tmpfs_fs_type.mount(args, result);
}

struct fs_driver devtmpfs_fs_type = {
	.name = "devtmpfs",
	.mount = devtmpfs_mount,
	.lock = SPINLOCK_INITIALIZER("devtmpfs_fs_lock"),
	.super_blocks = LIST_INITIALIZER(devtmpfs_fs_type.super_blocks),
	.list = LIST_INITIALIZER(devtmpfs_fs_type.list),
};
