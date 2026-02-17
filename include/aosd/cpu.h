#ifndef AOSD_CPU_H
#define AOSD_CPU_H

#include <aosd/sched_types.h>
#include <aosd/types.h>

struct cpu {
	struct cpu *next;
	uint32_t hart_id;
	struct task *current_task;
};

void cpu_add(struct cpu *cpu);
void save_cpu(uint32_t hart_id);
struct cpu *my_cpu(void);
void cpu_set_timebase_freq(uint32_t freq);
uint32_t cpu_get_timebase_freq(void);

#endif
