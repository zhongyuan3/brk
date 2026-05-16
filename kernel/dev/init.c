#include <brk/console.h>
#include <brk/dev.h>
#include <brk/panic.h>

extern void chrdev_registry_init(void);
extern void blkdev_registry_init(void);

void dev_init(void)
{
	chrdev_registry_init();
	blkdev_registry_init();

	if (console_register_dev())
		panic("console_register_dev failed\n");
	if (virtio_disk_init())
		panic("virtio_disk_init failed\n");
}
