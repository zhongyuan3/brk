#include <arch/mm.h>
#include <brk/asm/cpu.h>
#include <brk/console.h>
#include <brk/cpu.h>
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/printk.h>
#include <brk/task.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <riscv/csr.h>
#include <riscv/sbi.h>

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
	write_stvec((unsigned long)early_trap_vector);

	boot_info.boot_hart_id = hart_id;
	boot_info.dtb_pa = dtb_pa;
	boot_info.kernel_image_offset = kernel_image_offset;
	cpu_init(hart_id);

	console_register(&debug_console);
	klog_debug("debug_console registered\n");

	start_kernel();
}

void secondary_entry(unsigned long hart_id)
{
	write_stvec((unsigned long)early_trap_vector);

	load_final_page_table();

	cpu_init(hart_id);

	irq_init_hart(hart_id);
	/* Device IRQs are handled on the boot hart only (no IPI support yet). */
	trap_init_hart(hart_id);

	intr_on();

	klog_info("hart %lu ready\n", hart_id);

	task_scheduler();
}

void arch_init(void)
{
	paging_init();
	sbi_console_putstr("boot hart is ready\n");
	while (1) {
	}
}

static void smp_wake_secondary_harts(u64 init_hart_id)
{
	u64 start_addr = symbol_phys(_secondary_start);

	for (u64 id = 0; id < MAX_CPUS; ++id) {
		if (id == init_hart_id)
			continue;
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
