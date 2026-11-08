#define pr_fmt(fmt) "timer: " fmt
#include <arch/boot_info.h>
#include <arch/csr.h>
#include <arch/sbi.h>
#include <arch/timer.h>
#include <brk/clockevent.h>
#include <brk/clocksource.h>
#include <brk/cpu.h>
#include <brk/dt.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>

#define DEFAULT_TIMEBASE_FREQ 0x989680u

static u64 riscv_timer_read_cycles(void)
{
	return read_time();
}

static struct clocksource riscv_clocksource = {
	.name = "riscv-time",
	.read = riscv_timer_read_cycles,
	.rating = 100,
};

static u64 ns_to_cycles(u64 ns, u32 freq)
{
	u64 sec = ns / NS_PER_SEC;
	u64 rem = ns % NS_PER_SEC;

	return sec * freq + rem * freq / NS_PER_SEC;
}

static int riscv_timer_set_next_event(u64 delta_ns,
				      struct clock_event_device *dev)
{
	u64 cycles = ns_to_cycles(delta_ns, dev->frequency);

	sbi_set_timer(read_time() + cycles);
	return 0;
}

static struct clock_event_device riscv_clockevent = {
	.name = "riscv-timer",
	.rating = 100,
	.set_next_event = riscv_timer_set_next_event,
};

int timer_init(void)
{
	u32 freq;
	u64 timer_interval;
	u64 resolution_ns;
	int err;

	err = dt_get_timebase_freq(&freq);
	if (err) {
		freq = DEFAULT_TIMEBASE_FREQ;
		pr_warn("timebase-frequency unavailable (%d), using %u Hz\n",
			err, freq);
	}

	if (!freq)
		panic("timer: invalid timebase frequency\n");

	riscv_clocksource.frequency = freq;
	err = clocksource_register(&riscv_clocksource);
	if (err)
		panic("timer: clocksource_register failed: %d\n", err);

	riscv_clockevent.frequency = freq;
	resolution_ns = (NS_PER_SEC + freq - 1) / freq;
	riscv_clockevent.min_delta_ns = resolution_ns ? resolution_ns : 1;

	err = clockevents_register_device(&riscv_clockevent);
	if (err)
		panic("timer: clockevents_register_device failed: %d\n", err);

	timer_interval = freq / HZ;
	pr_debug("timer_interval=%lu (HZ=%d)\n", timer_interval, HZ);

	return 0;
}

void timer_init_hart(void)
{
	struct clock_event_device *dev = clockevents_get_current();

	if (!dev)
		return;

	(void)clockevents_program_event(dev, ktime_get_mono_ns() + TICK_NS);
}

void timer_handle_tick(void)
{
	struct clock_event_device *dev = clockevents_get_current();

	if (!dev)
		return;

	if (this_cpuid() == boot_info.boot_hart_id && dev->event_handler)
		dev->event_handler(dev);
	else
		(void)clockevents_program_event(dev,
						ktime_get_mono_ns() + TICK_NS);
}
