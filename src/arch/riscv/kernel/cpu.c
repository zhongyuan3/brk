#include <arch/csr.h>
#include <arch/hart.h>
#include <arch/irqstate.h>
#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/task.h>
#include <brk/task_types.h>

unsigned long nr_harts = 0;

const char *uname_machine(void)
{
	return "riscv64";
}

void cpu_idle(void)
{
	asm volatile("wfi");
}

void local_irq_disable(void)
{
	write_sstatus(read_sstatus() & ~SSTATUS_SIE);
}

void local_irq_enable(void)
{
	write_sstatus(read_sstatus() | SSTATUS_SIE);
}

irqstate_t local_irq_save(void)
{
	unsigned long old;
	unsigned long i = SSTATUS_SIE;
	asm volatile("csrrc %0, sstatus, %1" : "=r"(old) : "r"(i));
	return old;
}

void local_irq_restore(irqstate_t state)
{
	if ((state & SSTATUS_SIE) != 0)
		local_irq_enable();
}

bool local_irq_enabled(void)
{
	return read_sstatus() & SSTATUS_SIE;
}

void set_thread_pointer(unsigned long value)
{
	write_tp(value);
}

unsigned long get_thread_pointer(void)
{
	return read_tp();
}
