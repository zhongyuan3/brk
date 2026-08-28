#include <arch/irqflags.h>
#include <brk/base/types.h>
#include <brk/drivers/of.h>
#include <brk/drivers/plic.h>
#include <brk/drivers/rtc.h>
#include <brk/drivers/uart.h>
#include <brk/drivers/virtio_blk.h>
#include <brk/init/init.h>
#include <brk/init/initcall.h>
#include <brk/irq/irq.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/memblock.h>
#include <brk/mm/mm.h>
#include <brk/mm/pgalloc.h>
#include <brk/mm/vmalloc.h>
#include <brk/printk/console.h>
#include <brk/printk/printk.h>
#include <brk/process/processor.h>
#include <brk/process/task.h>
#include <brk/process/trap.h>
#include <brk/time/timekeeper.h>
#include <brk/time/timer.h>

void start_kernel(void)
{
	memblock_init();

	arch_init();

	page_alloc_init();
	memblock_free_all();

	kmalloc_init();
	vmalloc_init();

	console_register(&early_console);

	dtb_init_scan_cpu();
	dtb_init_scan_virtio_dev();

	plic_init();
	irq_init();
	rtc_init();
	timekeeper_init();
	timer_init();

	do_initcalls();

	cpuid_t hart_id = current_cpuid();
	irq_init_hart(hart_id);
	ns16550a_enable_irq(hart_id);
	virtio_blk_enable_irq(hart_id);
	trap_init_hart(hart_id);

	intr_on();

	task_scheduler();
}