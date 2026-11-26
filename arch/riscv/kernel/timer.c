#define pr_fmt(fmt) "timer: " fmt
#include <arch/timer.h>
#include <brk/clockevent.h>
#include <brk/clocksource.h>
#include <brk/cpu.h>
#include <brk/of.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/tick.h>
#include <brk/timeconst.h>
#include <riscv/csr.h>
#include <riscv/sbi.h>
#include <uapi/errno.h>

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

static int riscv_timer_set_next_event(u64 delta_ns,
				      struct clock_event_device *dev)
{
	u64 cycles;

	if (!dev)
		return -EINVAL;

	cycles = clocksource_ns_to_cycles_freq(dev->frequency, delta_ns);
	return (int)sbi_set_timer(read_time() + cycles);
}

static u32 riscv_timebase_freq;
static struct clock_event_device riscv_clockevents[MAX_CPUS];

static int riscv_clockevent_init(cpuid_t cpu)
{
	struct clock_event_device *dev;
	u64 resolution_ns;

	if (cpu >= MAX_CPUS)
		return -EINVAL;

	dev = &riscv_clockevents[cpu];
	dev->name = "riscv-timer";
	dev->rating = 100;
	dev->frequency = riscv_timebase_freq;
	resolution_ns = (NS_PER_SEC + dev->frequency - 1) / dev->frequency;
	dev->min_delta_ns = resolution_ns ? resolution_ns : 1;
	dev->set_next_event = riscv_timer_set_next_event;

	return clockevents_register_device(dev);
}

void timer_init(void)
{
	u32 freq;
	u64 timer_interval;
	int err;
	struct device_node *node;

	node = of_find_node_by_path("/cpus");
	if (!node || of_property_read_u32(node, "timebase-frequency", &freq))
		panic("timer: no timebase-frequency in /cpus\n");

	if (!freq)
		panic("timer: invalid timebase frequency\n");

	riscv_timebase_freq = freq;
	riscv_clocksource.frequency = freq;
	err = clocksource_register(&riscv_clocksource);
	if (err)
		panic("timer: clocksource_register failed: %d\n", err);

	err = riscv_clockevent_init(this_cpuid());
	if (err)
		panic("timer: clockevents_register_device failed: %d\n", err);

	timer_interval = freq / HZ;
	pr_debug("timer_interval=%lu (HZ=%d)\n", timer_interval, HZ);
}

void timer_init_hart(void)
{
	int err;

	err = riscv_clockevent_init(this_cpuid());
	if (err)
		panic("timer: failed to register clockevent on hart %lu: %d\n",
		      this_cpuid(), err);

	tick_init_cpu();
}

void timer_handle_tick(void)
{
	tick_handle_irq();
}
