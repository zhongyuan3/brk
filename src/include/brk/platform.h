#ifndef BRK_PLATFORM_H
#define BRK_PLATFORM_H

#include <brk/device.h>
#include <brk/kernel.h>
#include <brk/resource.h>
#include <brk/types.h>

/* Platform device: resource array, optional platform data and OF node. */
struct platform_device {
	struct device dev;
	struct resource *resources;
	unsigned int num_resources;
	const char *compatible;
	void *pdata;
};

struct platform_driver {
	struct device_driver driver;
};

extern struct bus_type platform_bus;

static inline struct platform_device *to_platform_device(struct device *dev)
{
	return container_of(dev, struct platform_device, dev);
}

int platform_device_register_simple(const char *name, const char *compatible,
				    u64 phys_base, u64 size, u32 irq,
				    void *pdata);
void platform_device_unregister(struct platform_device *pdev);
void platform_device_release(struct device *dev);
struct resource *platform_get_resource(struct platform_device *pdev,
				       unsigned int type, unsigned int index);
int platform_get_irq(struct platform_device *pdev, unsigned int index);

int platform_driver_register(struct platform_driver *pdrv);
void platform_driver_unregister(struct platform_driver *pdrv);

#endif
