#ifndef BRK_PLATFORM_H
#define BRK_PLATFORM_H

#include <brk/device_model.h>
#include <brk/kernel.h>
#include <brk/types.h>

/* Simple platform device: one MMIO window, one IRQ, optional platform data. */
struct platform_device {
	struct device dev;
	uint64_t phys_base;
	uint64_t size;
	uint32_t irq;
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
				    uint64_t phys_base, uint64_t size,
				    uint32_t irq, void *pdata);
void platform_device_unregister(struct platform_device *pdev);

int platform_driver_register(struct platform_driver *pdrv);
void platform_driver_unregister(struct platform_driver *pdrv);

#endif
