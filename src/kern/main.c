#include <brk/blkdev.h>
#include <brk/chrdev.h>
#include <brk/clocksource.h>
#include <brk/cpu.h>
#include <brk/dcache.h>
#include <brk/device.h>
#include <brk/fs.h>
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

/*
 * Every boot initializer is called explicitly here, in dependency order.
 * There is no linker-provided initcall table: adding initialization means
 * adding a call to this function.
 */
void start_kernel(void)
{
	int err;

	err = arch_init();
	if (err)
		panic("arch: initialization failed: err=%d\n", err);

	err = mm_init();
	if (err)
		panic("mm: initialization failed: err=%d\n", err);

	err = timer_init();
	if (err)
		panic("timer: initialization failed: err=%d\n", err);

	err = clocksource_init();
	if (err)
		panic("clocksource: initialization failed: err=%d\n", err);

	err = timekeeper_init();
	if (err)
		panic("timekeeper: initialization failed: err=%d\n", err);

	err = tick_init();
	if (err)
		panic("tick: initialization failed: err=%d\n", err);

	err = trap_init();
	if (err)
		panic("trap: initialization failed: err=%d\n", err);

	/* Core data structures, buses and registries. */
	cpu_sched_init();
	task_cache_init();
	irq_init();
	uvm_space_cache_init();
	page_cache_init();
	blkdev_registry_init();
	chrdev_registry_init();
	platform_bus_init();
	virtio_bus_init();
	plic_driver_register();
	fs_dentry_cache_init();
	fs_file_cache_init();
	fs_inode_cache_init();
	net_init();

	/* Device tree was built in arch_init(). */
	irqchip_early_init();

	err = of_platform_populate();
	if (err)
		pr_warn("of: platform populate failed: %d\n", err);

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

	task_scheduler();
}
