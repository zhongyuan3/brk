#ifndef BRK_CHRDEV_H
#define BRK_CHRDEV_H

#include <brk/device.h>
#include <brk/types.h>
#include <uapi/types.h>

struct opened_file_ops;

struct chrdev {
	dev_t dev;
	const struct opened_file_ops *fops;
	struct hlist_node hlist;
};

void chrdev_registry_init(void);

struct chrdev *chrdev_alloc(void);
void chrdev_free(struct chrdev *cd);

int chrdev_register(struct chrdev *cd);
void chrdev_unregister(struct chrdev *cd);
struct chrdev *chrdev_get(dev_t dev);

int chrdev_alloc_major(unsigned *major_out);
void chrdev_free_major(unsigned major);

int chrdev_alloc_minor(unsigned major, unsigned *minor_out);
void chrdev_free_minor(unsigned major, unsigned minor);

int chrdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out);
void chrdev_free_region(unsigned major, unsigned minor, unsigned count);

#endif
