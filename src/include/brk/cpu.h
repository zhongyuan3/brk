#ifndef BRK_CPU_H
#define BRK_CPU_H

#include <arch/context.h>
#include <arch/irqstate.h>
#include <brk/asm/cpu.h>
#include <brk/list.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

struct task;

struct cpu {
	struct task *handoff;
	struct task *current;
	struct task *idle;
	cpuid_t id;
	int preempt_count;
	bool initialized;
	/* Set once this CPU has entered its idle task and can take IPIs. */
	bool online;

	/* Per-CPU run queue. */
	struct list_head run_queue;
	spinlock_t run_queue_lock;
	unsigned int nr_running;
};

int cpu_init(cpuid_t cpuid);
void cpu_sched_init(void);
struct cpu *this_cpu(void);
cpuid_t this_cpuid(void);
cpuid_t this_cpuid_safe(void);
void set_this_cpuid(cpuid_t cpuid);

void preempt_disable(void);
void preempt_enable(void);
int preempt_count(void);

struct task *current_task(void);
void set_current_task(struct task *task);

extern struct cpu cpus[MAX_CPUS];

/* architecture-specific */

const char *uname_machine(void);
void cpu_idle(void);
void send_ipi(cpuid_t cpuid);

void local_irq_disable(void);
void local_irq_enable(void);
irqstate_t local_irq_save(void);
void local_irq_restore(irqstate_t state);
bool local_irq_enabled(void);

/*
 * Save the callee-saved state of @prev and resume @next.  Both tasks must be
 * distinct, and @prev must be the task currently running on this CPU.
 */
void context_switch(struct task *prev, struct task *next);

void set_thread_pointer(unsigned long value);
unsigned long get_thread_pointer(void);

#endif
