#include <aosd/cpu.h>
#include <aosd/dtb.h>
#include <aosd/panic.h>
#include <aosd/riscv.h>

static struct cpu *cpus;
static uint32_t timebase_freq;

void cpu_add(struct cpu *cpu)
{
	cpu->next = cpus;
	cpus = cpu;
}

void save_cpu(uint32_t hart_id)
{
	struct cpu *cpu = cpus;
	while (cpu) {
		if (cpu->hart_id == hart_id) {
			write_tp((uint64_t)cpu);
			return;
		}
		cpu = cpu->next;
	}

	panic("%s(): no cpu with hart id %u\n", __func__, hart_id);
}

struct cpu *my_cpu(void)
{
	return (struct cpu *)read_tp();
}

void cpu_set_timebase_freq(uint32_t freq)
{
	timebase_freq = freq;
}

uint32_t cpu_get_timebase_freq(void)
{
	return timebase_freq;
}
