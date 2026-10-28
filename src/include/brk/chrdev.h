#ifndef BRK_CHRDEV_H
#define BRK_CHRDEV_H

#include <brk/device.h>
#include <brk/refcnt_types.h>
#include <brk/types.h>
#include <uapi/types.h>

struct fs_file_ops;

struct char_dev {
	dev_t dev;
	/*
	 * Driver operations.  The VFS file keeps chrdev_fops as file->ops so
	 * the character core can hold a reference for the whole open
	 * lifetime; drivers must keep per-open state in file->private_data
	 * and must not replace file->ops.
	 */
	const struct fs_file_ops *fops;
	struct hlist_node hlist;
	refcnt_t ref;
	bool dead;
};

void chrdev_registry_init(void);

struct char_dev *chrdev_alloc(void);
void chrdev_free(struct char_dev *cd);

int chrdev_register(struct char_dev *cd);
void chrdev_unregister(struct char_dev *cd);
struct char_dev *chrdev_get(dev_t dev);
void chrdev_put(struct char_dev *cd);

int chrdev_alloc_major(unsigned *major_out);
void chrdev_free_major(unsigned major);

int chrdev_alloc_minor(unsigned major, unsigned *minor_out);
void chrdev_free_minor(unsigned major, unsigned minor);

int chrdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out);
void chrdev_free_region(unsigned major, unsigned minor, unsigned count);

#endif
