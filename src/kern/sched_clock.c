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
	__atomic_store_n(&sched_cycle_base, base, __ATOMIC_RELAXED);
	__atomic_store_n(&sched_cs, cs, __ATOMIC_RELEASE);
}

u64 sched_clock(void)
{
	struct clocksource *cs = __atomic_load_n(&sched_cs, __ATOMIC_ACQUIRE);
	u64 cycles;
	u64 sec;
	u64 rem;

	if (!cs)
		return 0;

	cycles = cs->read() -
		 __atomic_load_n(&sched_cycle_base, __ATOMIC_RELAXED);
	sec = cycles / cs->frequency;
	rem = cycles % cs->frequency;
	return sec * NS_PER_SEC + rem * NS_PER_SEC / cs->frequency;
}

u64 local_clock(void)
{
	return sched_clock();
}
