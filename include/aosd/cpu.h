#ifndef AOSD_CPU_H
#define AOSD_CPU_H

#include <aosd/process_types.h>
#include <aosd/riscv.h>
#include <aosd/types.h>

struct cpu {
	struct process *current;
	uint32_t hart_id;
	struct context ctx;
	short noff;
	bool intena;
};

void cpu_init_hart(uint32_t hart_id);
void cpu_set_timebase_freq(uint32_t freq);
uint32_t cpu_get_timebase_freq(void);

static inline struct cpu *current_cpu(void)
{
	return (struct cpu *)read_tp();
}

void push_off(void);
void pop_off(void);

#endif
