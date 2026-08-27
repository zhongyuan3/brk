#include <arch/csr.h>
#include <arch/mm.h>
#include <arch/pgtable.h>
#include <arch/sbi.h>
#include <arch/smp.h>
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/mm.h>
#include <brk/printk.h>
#include <brk/processor.h>
#include <brk/task.h>
#include <brk/trap.h>
#include <brk/types.h>

#if ENABLE_SMP

void start_hart(void)
{
	final_pgtable_enable();

	cpuid_t hart_id = current_cpuid();
	irq_init_hart(hart_id);
	/* Device IRQs are handled on the boot hart only (no IPI support yet). */
	trap_init_hart(hart_id);

	intr_on();

	klog_info("hart %d ready\n", hart_id);

	task_scheduler();
}

static void smp_wake_secondary_harts(uint64_t init_hart_id)
{
	uint64_t start_addr = symbol_phys(hart_entry);

	for (uint64_t id = 0; id < NR_CPUS; ++id) {
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
	smp_wake_secondary_harts(boot_cpuid);
}

#endif
