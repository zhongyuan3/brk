#include <arch/mm.h>
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
#include <brk/trap.h>
#include <brk/types.h>
#include <arch/csr.h>
#include <arch/hart.h>
#include <arch/sbi.h>

struct boot_info boot_info;

extern char early_trap_vector[];

static int debug_console_put_char(struct console *cons, int ch)
{
	(void)cons;
	sbi_console_putchar(ch);
	return 0;
}

static struct console debug_console = {
	.active = true,
	.put_char = debug_console_put_char,
};

void primary_entry(unsigned long hart_id, unsigned long dtb_pa,
		   unsigned long kernel_image_offset)
{
	cpu_init(hart_id);
	setup_trap_entry();

	boot_info.boot_hart_id = hart_id;
	boot_info.dtb_pa = dtb_pa;
	boot_info.kernel_image_offset = kernel_image_offset;

	console_register(&debug_console);
	klog_debug("debug_console registered\n");

	start_kernel();
}

void secondary_entry(unsigned long hart_id)
{
	cpu_init(hart_id);
	setup_trap_entry();

	load_final_page_table();

	klog_debug("smp: hart %lu online\n", hart_id);
	while (1) {
	}

	irq_init_hart(hart_id);
	/* Device IRQs are handled on the boot hart only (no IPI support yet). */
	trap_init_hart(hart_id);

	intr_on();

	klog_info("hart %lu ready\n", hart_id);

	task_scheduler();
}

void arch_init(void)
{
	klog_debug("dtb_pa=%#lx\n", boot_info.dtb_pa);
	paging_init();
	klog_debug("boot hart %lu is ready\n", this_cpuid());

	unsigned long dtb = phys_to_virt(boot_info.dtb_pa);
	dtb_scan_platform_devices(dtb);
	dtb_init_scan_cpu(dtb);
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
