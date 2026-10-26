#include <brk/device.h>
#include <brk/list.h>
#include <brk/printk.h>

static LIST_DEFINE(platform_devices);

void platform_device_add(struct platform_device *dev)
{
	list_add(&dev->list, &platform_devices);
	pr_debug("device: added platform device %s\n", dev->name);
}
