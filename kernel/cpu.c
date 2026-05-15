#include <brk/cpu.h>

static u32 timebase_freq;

void cpu_set_timebase_freq(u32 freq)
{
	timebase_freq = freq;
}

u32 cpu_get_timebase_freq(void)
{
	return timebase_freq;
}
