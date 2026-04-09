#include <brk/cpu.h>

static uint32_t timebase_freq;

void cpu_set_timebase_freq(uint32_t freq)
{
	timebase_freq = freq;
}

uint32_t cpu_get_timebase_freq(void)
{
	return timebase_freq;
}
