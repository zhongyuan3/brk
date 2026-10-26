#include <brk/clocksource.h>
#include <brk/dt.h>
#include <brk/printk.h>
#include <brk/types.h>

static u32 timebase_freq = 0x989680;

int clocksource_init(void)
{
	int ret = dt_setup_timebase_freq();
	if (ret == 0) {
		pr_debug("clocksource: initialized: timebase_freq=%u\n",
			   timebase_freq);
	}
	return ret;
}

void clocksource_set_timebase_freq(u32 freq)
{
	timebase_freq = freq;
}

u32 clocksource_get_timebase_freq(void)
{
	return timebase_freq;
}