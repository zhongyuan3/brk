#include <arch/irqflags.h>
#include <brk/blkdev.h>
#include <brk/chrdev.h>
#include <brk/console.h>
#include <brk/dcache.h>
#include <brk/device.h>
#include <brk/dtb.h>
#include <brk/fs.h>
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/processor.h>
#include <brk/rtc.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/uart.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/vmalloc.h>
#include <libfdt.h>

void start_kernel(void)
{
	memblock_init();

	arch_init();

	page_alloc_init();
	memblock_free_all();

	kmalloc_init();
	vmalloc_init();
	uvm_space_cache_init();

	console_register(&early_console);

	dtb_init_scan_cpu();
	dtb_init_scan_virtio_dev();

	irq_init();
	rtc_init();
	timekeeper_init();
	timer_init();

	chrdev_registry_init();
	blkdev_registry_init();
	ns16550a_driver_init();
	virtio_blk_init();
	dtb_init_scan_serial();
	virtio_init_scan();

	cpuid_t hart_id = current_cpuid();
	irq_init_hart(hart_id);
	ns16550a_enable_irq(hart_id);
	virtio_blk_enable_irq(hart_id);
	trap_init_hart(hart_id);

	page_cache_init();
	fs_dentry_cache_init();
	fs_inode_cache_init();
	fs_file_cache_init();
	task_cache_init();
	task_init_user();

	intr_on();

	task_scheduler();
}
