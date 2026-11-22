#include <brk/blkdev.h>
#include <brk/chrdev.h>
#include <brk/clocksource.h>
#include <brk/cpu.h>
#include <brk/dcache.h>
#include <brk/device.h>
#include <brk/fs.h>
#include <brk/futex.h>
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/irqchip.h>
#include <brk/mm.h>
#include <brk/net.h>
#include <brk/of.h>
#include <brk/pagecache.h>
#include <brk/panic.h>
#include <brk/platform.h>
#include <brk/printk.h>
#include <brk/rtc.h>
#include <brk/task.h>
#include <brk/tick.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/uart.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_mmio.h>
#include <brk/virtio_net.h>

void start_kernel(void)
{
	arch_init();
	mm_init();
	timer_init();
	clocksource_init();
	timekeeper_init();
	tick_init();
	trap_init();

	/* Core data structures, buses and registries. */
	cpu_sched_init();
	futex_init();
	process_cache_init();
	thread_cache_init();
	irq_init();
	uvm_space_cache_init();
	page_cache_init();
	blkdev_registry_init();
	chrdev_registry_init();
	platform_bus_init();
	virtio_bus_init();
	plic_driver_register();
	dentry_cache_init();
	file_cache_init();
	inode_cache_init();
	net_init();

	/* Device tree was built in arch_init(). */
	irqchip_early_init();

	of_platform_populate();

	/* Platform and transport drivers. */
	ns16550a_driver_init();
	virtio_mmio_platform_init();
	goldfish_rtc_driver_init();

	/* Leaf drivers. */
	virtio_blk_init();
	virtio_net_init();

	/* Retry devices that deferred their probe. */
	device_deferred_probe_init();

	irq_init_hart(this_cpuid());

	local_irq_enable();

	sched_start();
}
