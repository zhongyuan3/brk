#include <brk/clocksource.h>
#include <brk/types.h>

static u32 timebase_freq;

void clocksource_set_timebase_freq(u32 freq)
{
	timebase_freq = freq;
}

u32 clocksource_get_timebase_freq(void)
{
	return timebase_freq;
}