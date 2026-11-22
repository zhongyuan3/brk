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
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/of.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/task.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <stdbool.h>

struct boot_info boot_info;

void primary_entry(unsigned long hart_id, unsigned long dtb_pa,
		   unsigned long kernel_image_offset)
{
	cpu_init(hart_id, true);
	setup_trap_entry();

	of_early_set_fdt(dtb_pa);

	boot_info.boot_hart_id = hart_id;
	boot_info.dtb_pa = dtb_pa;
	boot_info.kernel_image_offset = kernel_image_offset;

	start_kernel();
}

void secondary_entry(unsigned long hart_id)
{
	cpu_init(hart_id, false);
	setup_trap_entry();

	load_final_page_table();

	timer_init_hart();

	trap_init();

	irq_init_hart(hart_id);

	local_irq_enable();

	pr_info("cpu@%lu online\n", hart_id);

	sched_start();
}

void arch_init(void)
{
	int err;

	err = vm_init();
	if (err)
		panic("vm_init() failed: err=%d\n", err);

	err = of_init();
	if (err)
		panic("of_init() failed: err=%d\n", err);

	err = of_scan_cpu();
	if (err)
		panic("of_scan_cpu() failed: err=%d\n", err);
}

static void smp_wake_secondary_harts(unsigned long boot_hart_id)
{
	phys_addr_t start_addr = symbol_phys(_secondary_start);

	for (unsigned long id = 0; id < nr_harts; ++id) {
		if (id != boot_hart_id)
			sbi_hart_start(id, start_addr, 0);
	}
}

void bring_up_secondaries(void)
{
	static bool released;

	if (released)
		return;
	released = true;
	smp_wake_secondary_harts(boot_info.boot_hart_id);
}

void send_ipi(cpuid_t cpuid)
{
	if (cpuid >= MAX_CPUS)
		return;
	if (!__atomic_load_n(&cpus[cpuid].online, __ATOMIC_ACQUIRE))
		return;

	/*
	 * SBI IPI sets SSIP on the target hart.  If the extension is not
	 * implemented the call is simply ignored; the target's timer tick
	 * will still re-examine its run queue.
	 */
	(void)sbi_send_ipi(1UL, cpus[cpuid].id);
}
