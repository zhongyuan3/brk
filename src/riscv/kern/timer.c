#include <arch/boot_info.h>
#include <arch/csr.h>
#include <arch/sbi.h>
#include <arch/timer.h>
#include <brk/clocksource.h>
#include <brk/dt.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/task.h>
#include <brk/tcp.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>

#define DEFAULT_TIMEBASE_FREQ 0x989680u

static u64 timer_interval;

static u64 riscv_timer_read_cycles(void)
{
	return read_time();
}

static struct clocksource riscv_clocksource = {
	.name = "riscv-time",
	.read = riscv_timer_read_cycles,
	.rating = 100,
};

void timer_clocksource_init(void)
{
	u32 freq;
	int err;

	err = dt_get_timebase_freq(&freq);
	if (err) {
		freq = DEFAULT_TIMEBASE_FREQ;
		pr_warn("timer: timebase-frequency unavailable (%d), using %u Hz\n",
			err, freq);
	}

	riscv_clocksource.frequency = freq;
	err = clocksource_register(&riscv_clocksource);
	if (err)
		panic("timer: clocksource_register failed: %d\n", err);
}

void timer_init(void)
{
	u32 timebase_freq = clocksource_get_frequency();

	if (!timebase_freq)
		panic("timer: no clocksource selected\n");

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
	if (this_cpuid() == boot_info.boot_hart_id) {
		timekeeper_tick();
		tcp_timer_tick();
	}

	task_wake_all(timekeeper_wait_chan());

	program_next_tick();
}
