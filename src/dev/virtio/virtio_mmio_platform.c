#include <brk/device.h>
#include <brk/initcall.h>
#include <brk/kernel.h>
#include <brk/panic.h>
#include <brk/platform.h>
#include <brk/printk.h>
#include <brk/resource.h>
#include <brk/virtio.h>
#include <uapi/errno.h>

static int virtio_mmio_platform_probe(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct virtio_device *vdev;
	struct resource *mem;
	int irq;
	int err;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	if (!mem || irq < 0)
		return -ENODEV;

	vdev = virtio_dev_create(mem->start, resource_size(mem), (u32)irq);
	if (!vdev)
		return -ENODEV;

	vdev->dev.parent = dev;

	err = virtio_dev_add(vdev);
	if (err) {
		virtio_dev_destroy(vdev);
		return err;
	}

	dev->driver_data = vdev;
	return 0;
}

static void virtio_mmio_platform_remove(struct device *dev)
{
	struct virtio_device *vdev = dev->driver_data;

	if (!vdev)
		return;

	dev->driver_data = NULL;
	virtio_dev_destroy(vdev);
}

static const struct device_id virtio_mmio_platform_ids[] = {
	{ .compatible = "virtio,mmio" },
	{},
};

static struct platform_driver virtio_mmio_platform_driver = {
	.driver = {
		.name = "virtio_mmio",
		.id_table = virtio_mmio_platform_ids,
		.probe = virtio_mmio_platform_probe,
		.remove = virtio_mmio_platform_remove,
	},
};

static void virtio_mmio_platform_init(void)
{
	int err = platform_driver_register(&virtio_mmio_platform_driver);

	if (err)
		panic("%s(): platform_driver_register failed: %d\n", __func__,
		      err);
}
subsys_initcall(virtio_mmio_platform_init);
