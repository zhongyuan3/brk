#include <arch/csr.h>
#include <brk/asm/cpu.h>
#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/initcall.h>
#include <brk/kernel.h>
#include <brk/panic.h>
#include <brk/spinlock.h>
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
	cpus[cpuid].preempt_count = 0;
	cpus[cpuid].initialized = true;
	set_thread_pointer((unsigned long)&cpus[cpuid]);
	return 0;
}

/*
 * Tasks may be queued onto a CPU before that CPU has been brought online
 * (e.g. per-CPU kernel threads created during initcalls). Make sure every
 * per-CPU run queue is usable from the start.
 */
static void cpu_sched_init(void)
{
	for (int i = 0; i < MAX_CPUS; ++i) {
		list_init(&cpus[i].run_queue);
		spinlock_init(&cpus[i].run_queue_lock, "rq");
		cpus[i].nr_running = 0;
		cpus[i].online = false;
	}
}
core_initcall(cpu_sched_init);

void preempt_disable(void)
{
	this_cpu()->preempt_count++;
	__asm__ volatile("" ::: "memory");
}

void preempt_enable(void)
{
	struct cpu *cpu = this_cpu();

	ASSERT(cpu->preempt_count > 0);
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

void set_this_cpuid(cpuid_t cpuid)
{
	(void)cpuid;
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
