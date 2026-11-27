#include <brk/atomic.h>
#include <brk/clocksource.h>
#include <brk/sched_clock.h>
#include <brk/timeconst.h>
#include <brk/types.h>

static struct clocksource *sched_cs;
static u64 sched_cycle_base;

void sched_clock_init(struct clocksource *cs)
{
	u64 base;

	if (!cs || !cs->read || !cs->frequency)
		return;

	base = cs->read();
	atomic_store_n(&sched_cycle_base, base, ATOMIC_RELAXED);
	atomic_store_n(&sched_cs, cs, ATOMIC_RELEASE);
}

u64 sched_clock(void)
{
	struct clocksource *cs = atomic_load_n(&sched_cs, ATOMIC_ACQUIRE);
	u64 cycles;

	if (!cs)
		return 0;

	cycles = cs->read() - atomic_load_n(&sched_cycle_base, ATOMIC_RELAXED);
	return clocksource_cycles_to_ns(cs, cycles);
}

u64 local_clock(void)
{
	return sched_clock();
}
