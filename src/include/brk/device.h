#ifndef BRK_DEVICE_H
#define BRK_DEVICE_H

#include <brk/list.h>
#include <brk/types.h>
#include <uapi/stat.h>

/*
 * Device number encoding (dev_t):
 *   bits 28..31 : device type (CHRDEV / BLKDEV)
 *   bits 20..27 : major (8 bits, 0..255)
 *   bits  0..19 : minor (20 bits, 0..1048575)
 */
#define MAJOR_BITS 8
#define MINOR_BITS 20
#define MAJOR_MAX (1u << MAJOR_BITS)
#define MINOR_MAX (1u << MINOR_BITS)
#define MAJOR_MASK (MAJOR_MAX - 1)
#define MINOR_MASK (MINOR_MAX - 1)

#define CHRDEV 0
#define BLKDEV ((u32)1 << 28)

#define MKDEV(major, minor) \
	((((major) & MAJOR_MASK) << 20) | ((minor) & MINOR_MASK))
#define MKCHRDEV(major, minor) (CHRDEV | MKDEV(major, minor))
#define MKBLKDEV(major, minor) (BLKDEV | MKDEV(major, minor))
#define DEVTYPE(dev) ((dev) & ((u32)15 << 28))
#define MAJOR(dev) (((dev) >> 20) & MAJOR_MASK)
#define MINOR(dev) ((dev) & MINOR_MASK)

#define IS_CHRDEV(dev) (DEVTYPE(dev) == CHRDEV)
#define IS_BLKDEV(dev) (DEVTYPE(dev) == BLKDEV)

#define FIRST_DYNAMIC_CHRDEV_MAJOR 16

#define NS16550A_MAJOR 1
#define NS16550A_MINOR_START 0
#define NS16550A_MINOR_COUNT 4

#define FIRST_DYNAMIC_BLKDEV_MAJOR 16

#define VIRTIO_BLK_MAJOR 1
#define VIRTIO_BLK_MINOR_START 0
#define VIRTIO_BLK_MAX_DEVS 4
/* Whole disk plus up to four primary partitions per device. */
#define VIRTIO_BLK_MINORS_PER_DEV 5
#define VIRTIO_BLK_MINOR_COUNT (VIRTIO_BLK_MAX_DEVS * VIRTIO_BLK_MINORS_PER_DEV)

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
struct device_node;

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
	struct device_node *of_node;

	struct list_head bus_list; /* bus->devices */
	struct list_head device_list; /* global device list */
	struct list_head children;
	struct list_head child_node; /* parent->children */

	void *platform_data;
	void *driver_data;

	dev_t devt; /* 0: no devtmpfs node */
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
