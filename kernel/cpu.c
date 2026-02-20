#include <aosd/cpu.h>
#include <aosd/dtb.h>
#include <aosd/panic.h>
#include <aosd/riscv.h>

static uint32_t timebase_freq;
static struct cpu cpus[NR_CPUS];

void cpu_init(uint32_t hart_id)
{
	cpus[hart_id].hart_id = hart_id;
	write_tp((uint64_t)&cpus[hart_id]);
}

struct cpu *current_cpu(void)
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
