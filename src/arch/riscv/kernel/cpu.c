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
	intr_off();
}

void local_irq_enable(void)
{
	intr_on();
}

irqstate_t local_irq_save(void)
{
	irqstate_t state = intr_off_get();
	return state;
}

void local_irq_restore(irqstate_t state)
{
	if (state > 0)
		intr_on();
}

void set_thread_pointer(unsigned long value)
{
	write_tp(value);
}

unsigned long get_thread_pointer(void)
{
	return read_tp();
}
