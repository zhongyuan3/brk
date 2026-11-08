#define pr_fmt(fmt) "clocksource: " fmt
#include <brk/clocksource.h>
#include <brk/printk.h>
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
	current_clocksource = best;
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

	pr_debug("selected %s (%u Hz, rating %u)\n", cs->name, cs->frequency,
		 cs->rating);
	return 0;
}

u64 clocksource_read_ns(void)
{
	struct clocksource *cs = current_clocksource;
	u64 cycles;
	u64 sec;
	u64 rem;

	if (!cs || !cs->frequency)
		return 0;

	cycles = cs->read();
	sec = cycles / cs->frequency;
	rem = cycles % cs->frequency;
	return sec * NS_PER_SEC + rem * NS_PER_SEC / cs->frequency;
}

u32 clocksource_get_frequency(void)
{
	return current_clocksource ? current_clocksource->frequency : 0;
}

u64 clocksource_get_resolution_ns(void)
{
	struct clocksource *cs = current_clocksource;
	u64 res;

	if (!cs || !cs->frequency)
		return TICK_NS;

	res = (NS_PER_SEC + cs->frequency - 1) / cs->frequency;
	return res ? res : 1;
}
