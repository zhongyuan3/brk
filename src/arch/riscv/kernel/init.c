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

void primary_entry(unsigned long hart_id, unsigned long dtb_pa,
		   unsigned long kernel_image_offset)
{
	cpu_init(hart_id);
	setup_trap_entry();

	dt_set_dtb(dtb_pa);

	boot_info.boot_hart_id = hart_id;
	boot_info.dtb_pa = dtb_pa;
	boot_info.kernel_image_offset = kernel_image_offset;

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

	pr_debug("smp: hart %lu online\n", hart_id);

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

void arch_send_ipi(cpuid_t cpuid)
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
