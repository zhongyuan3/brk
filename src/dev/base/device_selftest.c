#include <brk/container_of.h>
#include <brk/device.h>
#include <brk/kmalloc.h>
#include <brk/printk.h>
#include <brk/types.h>
#include <uapi/errno.h>

struct selftest_device {
	struct device dev;
};

static unsigned int failures;
static unsigned int release_count;
static unsigned int defer_probe_calls;
static unsigned int defer_remove_calls;

#define CHECK(cond, name)                                             \
	do {                                                          \
		if (!(cond)) {                                        \
			pr_err("device-selftest: %s failed\n", name); \
			failures++;                                   \
		}                                                     \
	} while (0)

static struct bus_type selftest_bus = {
	.name = "device-selftest",
};

static void selftest_device_release(struct device *dev)
{
	release_count++;
	kfree(container_of(dev, struct selftest_device, dev));
}

static int selftest_match(struct device *dev, struct device_driver *drv)
{
	(void)dev;
	(void)drv;
	return 1;
}

static int defer_probe(struct device *dev)
{
	(void)dev;

	defer_probe_calls++;
	if (defer_probe_calls == 1)
		return -EPROBE_DEFER;
	return 0;
}

static void defer_remove(struct device *dev)
{
	(void)dev;
	defer_remove_calls++;
}

static struct device_driver defer_driver = {
	.name = "device-selftest-defer",
	.bus = &selftest_bus,
	.probe = defer_probe,
	.remove = defer_remove,
};

static struct device_driver duplicate_driver = {
	.name = "device-selftest-defer",
	.bus = &selftest_bus,
	.probe = defer_probe,
};

static struct device_driver retry_driver = {
	.name = "device-selftest-retry",
	.bus = &selftest_bus,
};

static void test_deferred_probe(struct selftest_device *defer_dev)
{
	int err;

	device_init(&defer_dev->dev, "selftest-defer");
	defer_dev->dev.bus = &selftest_bus;
	defer_dev->dev.release = selftest_device_release;

	err = device_register(&defer_dev->dev);
	CHECK(err == 0, "deferred device register");
	CHECK(defer_probe_calls == 1, "first probe runs");
	CHECK(defer_dev->dev.state == DEVICE_STATE_DEFERRED,
	      "device is deferred");

	err = driver_register(&retry_driver);
	CHECK(err == 0, "retry driver register");
	CHECK(defer_probe_calls == 2, "deferred probe retried");
	CHECK(defer_dev->dev.state == DEVICE_STATE_BOUND, "device is bound");

	device_unregister(&defer_dev->dev);
	CHECK(defer_remove_calls == 1, "bound device is removed");
}

static void test_driver_duplicates(void)
{
	CHECK(driver_register(&defer_driver) == 0, "driver register");
	CHECK(driver_register(&defer_driver) == -EBUSY,
	      "re-registering a driver is rejected");
	CHECK(driver_register(&duplicate_driver) == -EEXIST,
	      "duplicate driver name is rejected");
}

static void test_snapshot_reference(void)
{
	struct selftest_device *dev;
	struct device **devices;
	unsigned int base = release_count;
	size_t count = 0;

	dev = kzalloc(sizeof(*dev));
	CHECK(dev != NULL, "snapshot device alloc");
	if (!dev)
		return;

	device_init(&dev->dev, "selftest-snapshot");
	dev->dev.release = selftest_device_release;

	CHECK(device_register(&dev->dev) == 0, "snapshot device register");

	devices = device_snapshot(&count);
	CHECK(devices != NULL, "device snapshot");
	CHECK(count > 0, "snapshot has devices");

	device_unregister(&dev->dev);
	CHECK(release_count == base, "snapshot holds a device reference");

	device_snapshot_free(devices, count);
	CHECK(release_count == base + 1, "last reference releases the device");
}

static void test_get_put(void)
{
	struct selftest_device *dev;
	unsigned int base = release_count;

	dev = kzalloc(sizeof(*dev));
	CHECK(dev != NULL, "get/put device alloc");
	if (!dev)
		return;

	device_init(&dev->dev, "selftest-getput");
	dev->dev.release = selftest_device_release;

	CHECK(device_get(&dev->dev) == &dev->dev, "device_get");
	device_put(&dev->dev);
	CHECK(release_count == base, "extra reference delays release");
	device_put(&dev->dev);
	CHECK(release_count == base + 1, "final reference releases the device");
}

void device_selftest_init(void)
{
	struct selftest_device *defer_dev;

	selftest_bus.match = selftest_match;
	CHECK(bus_register(&selftest_bus) == 0, "bus register");

	test_driver_duplicates();
	test_get_put();
	test_snapshot_reference();

	defer_dev = kzalloc(sizeof(*defer_dev));
	CHECK(defer_dev != NULL, "deferred device alloc");
	if (defer_dev)
		test_deferred_probe(defer_dev);

	driver_unregister(&retry_driver);
	driver_unregister(&defer_driver);

	if (failures)
		pr_err("device-selftest: %u failure(s)\n", failures);
	else
		pr_info("device-selftest: all tests passed\n");
}
