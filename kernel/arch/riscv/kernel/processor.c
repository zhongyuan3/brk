#include <arch/csr.h>
#include <brk/panic.h>
#include <brk/processor.h>
#include <brk/task.h>
#include <brk/task_types.h>

struct task_control_block *current_task(void)
{
	return (struct task_control_block *)read_tp();
}

cpuid_t current_cpuid(void)
{
	return read_sscratch();
}

struct cpu *current_cpu(void)
{
	return &cpus[current_cpuid()];
}

void push_off(void)
{
	int enabled = intr_off_get();
	struct cpu *cpu = current_cpu();

	if (cpu->irq_nest == 0)
		cpu->irq_enabled = enabled;
	cpu->irq_nest += 1;
}

void pop_off(void)
{
	struct cpu *cpu = current_cpu();

	if (intr_enabled())
		panic("%s(): interruptible\n", __func__);
	if (cpu->irq_nest < 1)
		panic("%s(): nesting level < 1\n", __func__);
	cpu->irq_nest -= 1;
	if (cpu->irq_nest == 0 && cpu->irq_enabled)
		intr_on();
}

void set_current_task(struct task_control_block *task)
{
	write_tp((uint64_t)task);
}

void set_current_cpuid(cpuid_t cpuid)
{
	write_sscratch(cpuid);
}

void arch_cpu_idle(void)
{
	asm volatile("wfi");
}

const char *arch_uname_machine(void)
{
	return "riscv64";
}
