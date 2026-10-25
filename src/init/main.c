#include <arch/irqflags.h>
#include <brk/cpu.h>
#include <brk/dt.h>
#include <brk/init.h>
#include <brk/initcall.h>
#include <brk/irq.h>
#include <brk/kmalloc.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/pgalloc.h>
#include <brk/plic.h>
#include <brk/printk.h>
#include <brk/rtc.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/uart.h>
#include <brk/virtio_blk.h>
#include <brk/vmalloc.h>

void start_kernel(void)
{
	arch_init();

	mm_core_init();

	klog_debug("release secondary harts\n");

	smp_boot_release_secondary_harts();

	while (1) {
	}

	dtb_init_scan_virtio_dev(0);

	plic_init();
	irq_init();
	rtc_init();
	timekeeper_init();
	timer_init();

	do_initcalls();

	cpuid_t hart_id = this_cpuid();
	irq_init_hart(hart_id);
	ns16550a_enable_irq(hart_id);
	virtio_blk_enable_irq(hart_id);
	trap_init_hart(hart_id);

	intr_on();

	task_scheduler();
}
