#include <arch/irqflags.h>
#include <brk/clocksource.h>
#include <brk/console.h>
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
#include <brk/stddef.h>
#include <brk/task.h>
#include <brk/task_types.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/uart.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_net.h>
#include <brk/vmalloc.h>

extern initcall_t __initcall_start[];
extern initcall_t __initcall_end[];

void start_kernel(void)
{
	arch_init();
	mm_core_init();

	clocksource_init();
	rtc_init();
	timekeeper_init();
	timer_init();

	irqchip_init();
	irq_init();

	trap_init();

	do_initcalls();

	irq_init_hart(this_cpuid());
	ns16550a_enable_irq(this_cpuid());
	virtio_blk_enable_irq(this_cpuid());
	virtio_net_enable_irq(this_cpuid());

	local_irq_enable();

	task_scheduler();
}

void do_initcalls(void)
{
	initcall_t *fn;

	for (fn = __initcall_start; fn < __initcall_end; ++fn)
		(*fn)();
}
