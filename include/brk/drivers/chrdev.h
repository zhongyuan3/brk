#ifndef BRK_CHRDEV_H
#define BRK_CHRDEV_H

#include <brk/drivers/device.h>
#include <brk/lib/types.h>
#include <uapi/types.h>

struct fs_file_ops;

struct char_dev {
	dev_t dev;
	const struct fs_file_ops *fops;
	struct hlist_node hlist;
};

void chrdev_registry_init(void);

struct char_dev *chrdev_alloc(void);
void chrdev_free(struct char_dev *cd);

int chrdev_register(struct char_dev *cd);
void chrdev_unregister(struct char_dev *cd);
struct char_dev *chrdev_get(dev_t dev);

int chrdev_alloc_major(unsigned *major_out);
void chrdev_free_major(unsigned major);

int chrdev_alloc_minor(unsigned major, unsigned *minor_out);
void chrdev_free_minor(unsigned major, unsigned minor);

int chrdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out);
void chrdev_free_region(unsigned major, unsigned minor, unsigned count);

#endif
