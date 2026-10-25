#include <arch/csr.h>
#include <arch/hart.h>
#include <arch/irqstate.h>
#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/task.h>
#include <brk/task_types.h>

unsigned long nr_harts = 1;

void set_thread_pointer(unsigned long value)
{
	write_tp(value);
}

unsigned long get_thread_pointer(void)
{
	return read_tp();
}

struct task *current_task(void)
{
	return (struct task *)(uintptr_t)read_tp();
}

void push_off(void)
{
	int enabled = intr_off_get();
	struct cpu *cpu = this_cpu();

	if (cpu->irq_nest == 0)
		cpu->irq_enabled = enabled;
	cpu->irq_nest += 1;
}

void pop_off(void)
{
	struct cpu *cpu = this_cpu();

	if (intr_enabled())
		panic("%s(): interruptible\n", __func__);
	if (cpu->irq_nest < 1)
		panic("%s(): nesting level < 1\n", __func__);
	cpu->irq_nest -= 1;
	if (cpu->irq_nest == 0 && cpu->irq_enabled)
		intr_on();
}

void set_current_task(struct task *task)
{
	write_tp((uintptr_t)task);
}

void cpu_idle(void)
{
	asm volatile("wfi");
}

const char *uname_machine(void)
{
	return "riscv64";
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
