#include <brk/container_of.h>
#include <brk/device.h>
#include <brk/virtio.h>
#include <stdbool.h>
#include <uapi/errno.h>

struct bus_type virtio_bus = {
	.name = "virtio",
};

static inline struct virtio_device *to_virtio_device(struct device *dev)
{
	return container_of(dev, struct virtio_device, dev);
}

static bool virtio_match(struct device *dev, struct device_driver *drv)
{
	struct virtio_device *vdev = to_virtio_device(dev);
	const struct device_id *id;

	if (!drv->id_table)
		return false;

	for (id = drv->id_table; id->device_id; ++id) {
		if (id->device_id == vdev->id)
			return true;
	}
	return false;
}

void virtio_bus_init(void)
{
	virtio_bus.match = virtio_match;
	bus_register(&virtio_bus);
}

int virtio_dev_add(struct virtio_device *dev)
{
	/* On failure the caller owns cleanup via virtio_mmio_dev_destroy(). */
	return device_register(&dev->dev);
}

void virtio_dev_remove(struct virtio_device *dev)
{
	device_unregister(&dev->dev);
}

void virtio_reset(struct virtio_device *vdev)
{
	if (vdev && vdev->ops && vdev->ops->set_status)
		(void)vdev->ops->set_status(vdev, 0);
}

int virtio_start_driver(struct virtio_device *vdev)
{
	u32 status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

	if (!vdev || !vdev->ops || !vdev->ops->set_status)
		return -EINVAL;
	return vdev->ops->set_status(vdev, status);
}

u32 virtio_read_features(struct virtio_device *vdev)
{
	if (!vdev || !vdev->ops || !vdev->ops->get_features)
		return 0;
	return vdev->ops->get_features(vdev);
}

int virtio_write_features(struct virtio_device *vdev, u32 features)
{
	if (!vdev || !vdev->ops || !vdev->ops->set_features)
		return -EINVAL;
	return vdev->ops->set_features(vdev, features);
}

int virtio_features_ok(struct virtio_device *vdev)
{
	u32 status;
	int err;

	if (!vdev || !vdev->ops || !vdev->ops->get_status ||
	    !vdev->ops->set_status)
		return -EINVAL;

	status = vdev->ops->get_status(vdev);
	err = vdev->ops->set_status(vdev, status | VIRTIO_STATUS_FEATURES_OK);
	if (err)
		return err;

	status = vdev->ops->get_status(vdev);
	if (!(status & VIRTIO_STATUS_FEATURES_OK))
		return -EOPNOTSUPP;
	return 0;
}

int virtio_setup_queue(struct virtio_device *vdev, u32 queue_id,
		       struct virtq *vq, u32 queue_size)
{
	if (!vdev || !vdev->ops || !vdev->ops->setup_queue)
		return -EINVAL;
	return vdev->ops->setup_queue(vdev, queue_id, vq, queue_size);
}

int virtio_driver_ok(struct virtio_device *vdev)
{
	u32 status;

	if (!vdev || !vdev->ops || !vdev->ops->get_status ||
	    !vdev->ops->set_status)
		return -EINVAL;

	status = vdev->ops->get_status(vdev);
	return vdev->ops->set_status(vdev, status | VIRTIO_STATUS_DRIVER_OK);
}

void virtio_queue_notify(struct virtio_device *vdev, u32 queue_id)
{
	if (vdev && vdev->ops && vdev->ops->notify)
		vdev->ops->notify(vdev, queue_id);
}

u32 virtio_ack_interrupt(struct virtio_device *vdev)
{
	if (!vdev || !vdev->ops || !vdev->ops->ack_interrupt)
		return 0;
	return vdev->ops->ack_interrupt(vdev);
}

int virtio_read_config(struct virtio_device *vdev, void *buf, size_t size,
		       u32 offset)
{
	if (!vdev || !vdev->ops || !vdev->ops->read_config)
		return -EINVAL;
	return vdev->ops->read_config(vdev, buf, size, offset);
}
