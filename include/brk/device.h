#ifndef BRK_DEVICE_H
#define BRK_DEVICE_H

#include <brk/types.h>

/*
 * Device number encoding (dev_t):
 *   bits 28..31 : device type (CHRDEV / BLKDEV)
 *   bits 20..27 : major (8 bits, 0..255)
 *   bits  0..19 : minor (20 bits)
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

#define VIRTIO_DISK_MAJOR 1
#define VIRTIO_DISK_MINOR_START 0
#define VIRTIO_DISK_MINOR_COUNT 4

#endif
