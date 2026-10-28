#ifndef BRK_DEVICE_MODEL_H
#define BRK_DEVICE_MODEL_H

#include <brk/list.h>
#include <brk/types.h>
#include <uapi/stat.h>

/*
 * Minimal Linux-style device model.
 *
 * A device can exist on a bus (and be bound to a driver), and/or expose a
 * dev_t that devtmpfs materialises as /dev/<name>.  Buses own the device and
 * driver lists; matching is bus-specific.
 */
#define DEVICE_NAME_LEN 32

struct bus_type;
struct device_driver;

struct device_id {
	/* platform_bus: DT compatible string */
	const char *compatible;
	/* virtio_bus: virtio device ID; 0 terminates the table */
	u32 device_id;
};

struct device {
	char name[DEVICE_NAME_LEN];
	struct device *parent;
	struct bus_type *bus;
	struct device_driver *driver;

	struct list_head bus_list;    /* bus->devices */
	struct list_head device_list; /* global device list */
	struct list_head children;
	struct list_head child_node;  /* parent->children */

	void *platform_data;
	void *driver_data;

	dev_t devt;   /* 0: no devtmpfs node */
	umode_t mode; /* S_IFCHR/S_IFBLK | permissions */
	bool registered;
};

struct device_driver {
	const char *name;
	struct bus_type *bus;
	const struct device_id *id_table;
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	struct list_head bus_list;
};

struct bus_type {
	const char *name;
	int (*match)(struct device *dev, struct device_driver *drv);
	struct list_head devices;
	struct list_head drivers;
	struct list_head list;
};

void device_init(struct device *dev, const char *name);
int device_register(struct device *dev);
void device_unregister(struct device *dev);

int bus_register(struct bus_type *bus);
int driver_register(struct device_driver *drv);
void driver_unregister(struct device_driver *drv);

/*
 * Snapshot the registered device list.  Returns NULL when there are no
 * devices; otherwise the caller frees the array with device_snapshot_free().
 * The device objects themselves are not referenced and must not be freed by
 * the caller.
 */
struct device **device_snapshot(size_t *count);
void device_snapshot_free(struct device **devices);

#endif
