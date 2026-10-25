#include <arch/csr.h>
#include <arch/sbi.h>
#include <brk/clocksource.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <arch/boot_info.h>

static u64 timer_interval;

void timer_init(void)
{
	u32 timebase_freq = clocksource_get_timebase_freq();
	timer_interval = timebase_freq / 1000;
}

u64 timer_get_time(void)
{
	return read_time();
}

void timer_set_next(void)
{
	sbi_set_timer(timer_get_time() + timer_interval);
}

void timer_handle_int(void)
{
	if (this_cpuid() == boot_info.boot_hart_id)
		timekeeper_tick();

	task_wake_all(timekeeper_wait_chan());

	timer_set_next();
}
