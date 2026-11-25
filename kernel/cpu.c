#include <brk/asm/cpu.h>
#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>

struct cpu cpus[MAX_CPUS];

int cpu_init(cpuid_t cpuid, bool boot)
{
	if (boot) {
		for (int i = 0; i < MAX_CPUS; ++i) {
			list_init(&cpus[i].run_queue);
			spinlock_init(&cpus[i].run_queue_lock, "rq");
		}
	}

	if (cpuid >= MAX_CPUS)
		panic("cpu: cpu id %lu is out of range (MAX_CPUS = %d)\n",
		      cpuid, MAX_CPUS);

	cpus[cpuid].id = cpuid;
	cpus[cpuid].preempt_count = 0;
	cpus[cpuid].initialized = true;
	set_thread_pointer((unsigned long)&cpus[cpuid]);
	return 0;
}

void preempt_disable(void)
{
	this_cpu()->preempt_count++;
	__asm__ volatile("" ::: "memory");
}

void preempt_enable(void)
{
	struct cpu *cpu = this_cpu();

	assert(cpu->preempt_count > 0);
	cpu->preempt_count--;
	__asm__ volatile("" ::: "memory");
}

int preempt_count(void)
{
	return this_cpu()->preempt_count;
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

/*
 * Panic-safe CPU id: returns -1 if the per-CPU pointer has not been set up
 * yet.  Unlike this_cpuid(), this must never call panic().
 */
cpuid_t this_cpuid_safe(void)
{
	unsigned long ptr = get_thread_pointer();

	if (!cpu_is_valid(ptr))
		return (cpuid_t)-1;
	return ((struct cpu *)ptr)->id;
}

void set_this_cpuid(cpuid_t cpuid)
{
	(void)cpuid;
}

struct thread *current_thread(void)
{
	struct cpu *c = this_cpu();
	assert(c != NULL);
	return c->current;
}

void set_current_thread(struct thread *task)
{
	struct cpu *c = this_cpu();
	assert(c != NULL);
	c->current = task;
}
