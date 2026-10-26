#include <arch/csr.h>
#include <brk/asm/cpu.h>
#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/kernel.h>
#include <brk/panic.h>
#include <brk/stddef.h>
#include <brk/types.h>
#include <uapi/errno.h>

struct cpu cpus[MAX_CPUS];

int cpu_init(cpuid_t cpuid)
{
	if (cpuid >= MAX_CPUS) {
		panic("cpu: cpu id %lu is out of range (MAX_CPUS = %d)\n",
		      cpuid, MAX_CPUS);
	}

	cpus[cpuid].id = cpuid;
	cpus[cpuid].initialized = true;
	set_thread_pointer((unsigned long)&cpus[cpuid]);
	return 0;
}

static bool cpu_is_valid(unsigned long cpu_ptr)
{
	unsigned long base = (unsigned long)cpus;
	unsigned long end = base + MAX_CPUS * sizeof(struct cpu);
	return !(cpu_ptr % alignof(struct cpu) != 0 || cpu_ptr < base ||
		 cpu_ptr >= end);
}

struct cpu *this_cpu(void)
{
	unsigned long ptr = get_thread_pointer();
	if (!cpu_is_valid(ptr)) {
		panic("cpu: invalid cpu @%lu (the CPU might not be initialized)\n",
		      ptr);
	}
	return (struct cpu *)ptr;
}

cpuid_t this_cpuid(void)
{
	return this_cpu()->id;
}

void set_this_cpuid(cpuid_t cpuid)
{
	(void)cpuid;
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

struct task *current_task(void)
{
	struct cpu *c = this_cpu();
	ASSERT(c != NULL);
	return c->current;
}

void set_current_task(struct task *task)
{
	struct cpu *c = this_cpu();
	ASSERT(c != NULL);
	c->current = task;
}
