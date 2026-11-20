#define pr_fmt(fmt) "clocksource: " fmt
#include <brk/clocksource.h>
#include <brk/printk.h>
#include <brk/sched_clock.h>
#include <brk/spinlock.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <uapi/errno.h>

static LIST_DEFINE(clocksources);
static SPINLOCK_DEFINE(clocksource_lock);
static struct clocksource *current_clocksource;

int clocksource_register(struct clocksource *cs)
{
	struct clocksource *existing;
	irqstate_t flags;

	if (!cs || !cs->name || !cs->read || !cs->frequency)
		return -EINVAL;

	flags = spin_lock_irqsave(&clocksource_lock);
	list_for_each_entry(existing, &clocksources, list) {
		if (existing == cs) {
			spin_unlock_irqrestore(&clocksource_lock, flags);
			return -EBUSY;
		}
	}
	list_add_tail(&cs->list, &clocksources);
	spin_unlock_irqrestore(&clocksource_lock, flags);
	return 0;
}

struct clocksource *clocksource_select(void)
{
	struct clocksource *cs, *best = NULL;
	irqstate_t flags;

	flags = spin_lock_irqsave(&clocksource_lock);
	list_for_each_entry(cs, &clocksources, list) {
		if (!best || cs->rating > best->rating)
			best = cs;
	}
	__atomic_store_n(&current_clocksource, best, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&clocksource_lock, flags);
	return best;
}

int clocksource_init(void)
{
	struct clocksource *cs = clocksource_select();

	if (!cs) {
		pr_err("no source registered\n");
		return -ENODEV;
	}

	/*
	 * Establish the boot-clock epoch before the first timestamped message
	 * below.  This clock is independent from CLOCK_MONOTONIC's epoch,
	 * which timekeeper_init() establishes later.
	 */
	sched_clock_init(cs);

	pr_debug("selected %s (%u Hz, rating %u)\n", cs->name, cs->frequency,
		 cs->rating);
	return 0;
}

struct clocksource *clocksource_get_current(void)
{
	return __atomic_load_n(&current_clocksource, __ATOMIC_ACQUIRE);
}

u32 clocksource_get_frequency(void)
{
	struct clocksource *cs = clocksource_get_current();

	return cs ? cs->frequency : 0;
}

u64 clocksource_get_resolution_ns(void)
{
	struct clocksource *cs = clocksource_get_current();
	u64 res;

	if (!cs || !cs->frequency)
		return TICK_NS;

	res = (NS_PER_SEC + cs->frequency - 1) / cs->frequency;
	return res ? res : 1;
}
