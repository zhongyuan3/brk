#ifndef BRK_DEVICE_H
#define BRK_DEVICE_H

#include <brk/dev_t.h>
#include <brk/list.h>
#include <brk/refcnt_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <stdbool.h>
#include <uapi/stat.h>

/*
 * Minimal Linux-style device model.
 *
 * A device can exist on a bus (and be bound to a driver), and/or expose a
 * dev_t that devtmpfs materialises as /dev/<name>.  Buses own the device and
 * driver lists; matching is bus-specific.
 */
#define DEVICE_NAME_LEN 32

/*
 * Kernel-internal probe deferral.  This is deliberately not a user ABI
 * errno: drivers return it from probe to ask the core to retry later, once
 * more drivers/devices have registered.
 */
#define EPROBE_DEFER 517

struct bus_type;
struct device;
struct device_driver;
struct device_node;

typedef void (*device_release_t)(struct device *dev);

enum device_state {
	DEVICE_STATE_NEW = 0, /* initialized, not on any list */
	DEVICE_STATE_ADDED, /* registered, not bound */
	DEVICE_STATE_PROBING, /* driver->probe() is running */
	DEVICE_STATE_BOUND, /* bound to a driver */
	DEVICE_STATE_DEFERRED, /* probe asked to retry later */
	DEVICE_STATE_REMOVED, /* removed from the lists */
};

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
	struct list_head deferred_node; /* global deferred-probe list */

	void *platform_data;
	void *driver_data;

	dev_t devt; /* 0: no devtmpfs node */
	umode_t mode; /* S_IFCHR/S_IFBLK | permissions */

	refcnt_t refcnt;
	spinlock_t lock; /* protects driver/state/registered */
	enum device_state state;
	device_release_t release;
	bool registered;
};

struct device_driver {
	const char *name;
	struct bus_type *bus;
	const struct device_id *id_table;
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	struct list_head bus_list;
	bool registered;
};

struct bus_type {
	const char *name;
	bool (*match)(struct device *dev, struct device_driver *drv);
	struct list_head devices;
	struct list_head drivers;
	struct list_head list;
};

void device_init(struct device *dev, const char *name);

/*
 * Lifecycle:
 *   device_init()       -> initial reference (count 1)
 *   device_register()   -> publish and probe
 *   device_unregister() -> unbind, unlist and drop initial reference
 *
 * A device with a release callback is freed by the core when the last
 * reference goes away.  Devices without one keep the historical caller-owned
 * lifetime: device_unregister() drops the registration reference but does not
 * free the object.
 *
 * Removal does not sleep waiting for an in-flight probe: probe completion
 * finishes the unbind.  Device and driver storage must stay valid until any
 * probe that already started has returned.
 */
int device_register(struct device *dev);
void device_unregister(struct device *dev);

struct device *device_get(struct device *dev);
void device_put(struct device *dev);

int bus_register(struct bus_type *bus);
int driver_register(struct device_driver *drv);
void driver_unregister(struct device_driver *drv);

/*
 * Snapshot the registered device list.  Returns NULL when there are no
 * devices; otherwise the caller frees the array and drops the per-device
 * references with device_snapshot_free(devices, count).
 */
struct device **device_snapshot(size_t *count);
void device_snapshot_free(struct device **devices, size_t count);

/* Retry devices that deferred their probe during registration. */
void device_deferred_probe_init(void);

#endif
