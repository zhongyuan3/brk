#include <aosd/asm.h>
#include <aosd/cpu.h>
#include <aosd/dtb.h>
#include <aosd/panic.h>
#include <aosd/riscv.h>

static uint32_t timebase_freq;
static struct cpu cpus[NR_CPUS];

void cpu_init_hart(uint32_t hart_id)
{
	cpus[hart_id].hart_id = hart_id;
	write_tp((uint64_t)&cpus[hart_id]);
}

void cpu_set_timebase_freq(uint32_t freq)
{
	timebase_freq = freq;
}

uint32_t cpu_get_timebase_freq(void)
{
	return timebase_freq;
}

void push_off(void)
{
	bool intena = intr_enabled();
	intr_off();
	if (current_cpu()->noff == 0)
		current_cpu()->intena = intena;
	current_cpu()->noff += 1;
}

void pop_off(void)
{
	struct cpu *cpu = current_cpu();
	if (intr_enabled())
		panic("pop_off: interruptible\n");
	if (cpu->noff < 1)
		panic("pop_off\n");
	cpu->noff -= 1;
	if (cpu->noff == 0 && cpu->intena)
		intr_on();
}
