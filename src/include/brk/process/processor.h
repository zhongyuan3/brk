#ifndef BRK_PROCESSOR_H
#define BRK_PROCESSOR_H

#include <brk/base/types.h>

struct cpu;
struct task_control_block;

struct task_control_block *current_task(void);
cpuid_t current_cpuid(void);
struct cpu *current_cpu(void);
void set_current_task(struct task_control_block *task);
void set_current_cpuid(cpuid_t cpuid);

void push_off(void);
void pop_off(void);

void arch_cpu_idle(void);

const char *arch_uname_machine(void);

#endif
