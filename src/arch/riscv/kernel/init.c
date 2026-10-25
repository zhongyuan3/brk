#include <arch/csr.h>
#include <arch/hart.h>
#include <arch/mm.h>
#include <arch/sbi.h>
#include <arch/timer.h>
#include <arch/trap.h>
#include <arch/trapframe.h>
#include <brk/asm/cpu.h>
#include <brk/console.h>
#include <brk/cpu.h>
#include <brk/dt.h>
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/printk.h>
#include <brk/task.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>

struct boot_info boot_info;

extern char early_trap_vector[];

// static int debug_console_put_char(struct console *cons, int ch)
// {
// 	(void)cons;
// 	sbi_console_putchar(ch);
// 	return 0;
// }

// static struct console debug_console = {
// 	.active = true,
// 	.put_char = debug_console_put_char,
// };

void primary_entry(unsigned long hart_id, unsigned long dtb_pa,
		   unsigned long kernel_image_offset)
{
	cpu_init(hart_id);
	setup_trap_entry();

	dt_set_dtb(dtb_pa);

	boot_info.boot_hart_id = hart_id;
	boot_info.dtb_pa = dtb_pa;
	boot_info.kernel_image_offset = kernel_image_offset;

	// console_register(&debug_console);
	// klog_debug("debug_console registered\n");

	start_kernel();
}

void secondary_entry(unsigned long hart_id)
{
	cpu_init(hart_id);
	setup_trap_entry();

	load_final_page_table();

	timer_init_hart();

	trap_init();

	local_irq_enable();

	klog_debug("smp: hart %lu online\n", hart_id);
	while (1) {
	}

	task_scheduler();
}

void arch_init(void)
{
	paging_init();
	dt_init_scan_cpu();
}

static void smp_wake_secondary_harts(unsigned long boot_hart_id)
{
	unsigned long start_addr = symbol_phys(_secondary_start);

	for (unsigned long id = 0; id < nr_harts; ++id) {
		if (id != boot_hart_id)
			sbi_hart_start(id, start_addr, 0);
	}
}

void smp_boot_release_secondary_harts(void)
{
	static bool released;

	if (released)
		return;
	released = true;
	smp_wake_secondary_harts(boot_info.boot_hart_id);
}
