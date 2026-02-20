#ifndef AOSD_CPU_H
#define AOSD_CPU_H

#include <aosd/sched_types.h>
#include <aosd/types.h>

#define NR_CPUS 8

struct cpu {
	struct task *current;
	uint32_t hart_id;
};

void cpu_init(uint32_t hart_id);
struct cpu *current_cpu(void);
void cpu_set_timebase_freq(uint32_t freq);
uint32_t cpu_get_timebase_freq(void);

#endif
