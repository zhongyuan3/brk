#include <arch/boot_info.h>
#include <arch/csr.h>
#include <arch/sbi.h>
#include <arch/timer.h>
#include <brk/clocksource.h>
#include <brk/printk.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>

static u64 timer_interval;

void timer_init(void)
{
	u32 timebase_freq = clocksource_get_timebase_freq();
	timer_interval = timebase_freq / 1000;
	pr_debug("timer: timer_interval=%lu\n", timer_interval);
	timer_init_hart();
}

void timer_init_hart(void)
{
	program_next_tick();
}

u64 timer_read(void)
{
	return read_time();
}

void program_next_tick(void)
{
	sbi_set_timer(timer_read() + timer_interval);
}

void handle_tick(void)
{
	if (this_cpuid() == boot_info.boot_hart_id)
		timekeeper_tick();

	task_wake_all(timekeeper_wait_chan());

	program_next_tick();
}
