#include <arch/irqflags.h>
#include <brk/base/kernel.h>
#include <brk/base/types.h>
#include <brk/drivers/blkdev.h>
#include <brk/drivers/chrdev.h>
#include <brk/drivers/device.h>
#include <brk/drivers/plic.h>
#include <brk/drivers/rtc.h>
#include <brk/drivers/uart.h>
#include <brk/drivers/virtio.h>
#include <brk/drivers/virtio_blk.h>
#include <brk/fs/dcache.h>
#include <brk/fs/fs.h>
#include <brk/init/dtb.h>
#include <brk/init/init.h>
#include <brk/irq/irq.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/memblock.h>
#include <brk/mm/mm.h>
#include <brk/mm/pagecache.h>
#include <brk/mm/pgalloc.h>
#include <brk/mm/vmalloc.h>
#include <brk/printk/console.h>
#include <brk/printk/printk.h>
#include <brk/process/processor.h>
#include <brk/process/task.h>
#include <brk/process/trap.h>
#include <brk/time/timekeeper.h>
#include <brk/time/timer.h>
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

	plic_init();
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
