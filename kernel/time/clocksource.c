#include <brk/base/types.h>
#include <brk/time/clocksource.h>

static uint32_t timebase_freq;

void clocksource_set_timebase_freq(uint32_t freq)
{
	timebase_freq = freq;
}

uint32_t clocksource_get_timebase_freq(void)
{
	return timebase_freq;
}