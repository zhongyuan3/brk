#include <brk/clocksource.h>
#include <brk/types.h>

static uint32_t timebase_freq;

void clocksource_set_timebase_freq(uint32_t freq)
{
	timebase_freq = freq;
}

uint32_t clocksource_get_timebase_freq(void)
{
	return timebase_freq;
}