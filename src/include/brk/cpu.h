#ifndef BRK_CPU_H
#define BRK_CPU_H

#include <arch/context.h>
#include <arch/irqstate.h>
#include <brk/asm/cpu.h>
#include <brk/types.h>

struct task;

struct cpu {
	struct task *handoff;
	struct task *current;
	struct context ctx;
	unsigned long kernel_sp;
	cpuid_t id;
	int irq_nest;
	bool irq_enabled;
	bool initialized;
};

int cpu_init(cpuid_t cpuid);
struct cpu *this_cpu(void);
cpuid_t this_cpuid(void);
void set_this_cpuid(cpuid_t cpuid);

void push_off(void);
void pop_off(void);

struct task *current_task(void);
void set_current_task(struct task *task);

extern struct cpu cpus[MAX_CPUS];

/* architecture-specific */

const char *uname_machine(void);
void cpu_idle(void);

void local_irq_disable(void);
void local_irq_enable(void);
irqstate_t local_irq_save(void);
void local_irq_restore(irqstate_t state);

void switch_context(struct context *prev, struct context *next);
void __switch_to(unsigned long *old_sp, unsigned long *new_sp);
void set_thread_pointer(unsigned long value);
unsigned long get_thread_pointer(void);

#endif
