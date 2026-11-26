#include <brk/clockevent.h>
#include <brk/cpu.h>
#include <brk/spinlock.h>
#include <brk/timekeeper.h>
#include <brk/types.h>
#include <uapi/errno.h>

static LIST_DEFINE(clockevents);
static SPINLOCK_DEFINE(clockevents_lock);
static struct clock_event_device *current_clockevents[MAX_CPUS];

int clockevents_register_device(struct clock_event_device *dev)
{
	struct clock_event_device *existing;
	cpuid_t cpu = this_cpuid();
	irqstate_t flags;

	if (!dev || !dev->name || !dev->set_next_event || !dev->frequency)
		return -EINVAL;
	if (cpu >= MAX_CPUS)
		return -EINVAL;

	flags = spin_lock_irqsave(&clockevents_lock);
	list_for_each_entry(existing, &clockevents, list) {
		if (existing == dev) {
			spin_unlock_irqrestore(&clockevents_lock, flags);
			return -EBUSY;
		}
	}

	list_add_tail(&dev->list, &clockevents);
	dev->registered = true;

	if (!current_clockevents[cpu] ||
	    dev->rating > current_clockevents[cpu]->rating)
		__atomic_store_n(&current_clockevents[cpu], dev,
				 __ATOMIC_RELEASE);

	spin_unlock_irqrestore(&clockevents_lock, flags);
	return 0;
}

struct clock_event_device *clockevents_get_current(void)
{
	cpuid_t cpu = this_cpuid();

	if (cpu >= MAX_CPUS)
		return NULL;
	return __atomic_load_n(&current_clockevents[cpu], __ATOMIC_ACQUIRE);
}

int clockevents_set_mode(struct clock_event_device *dev,
			 enum clock_event_mode mode)
{
	if (!dev)
		return -EINVAL;
	if (!dev->set_mode)
		return 0;

	return dev->set_mode(mode, dev);
}

int clockevents_program_event(struct clock_event_device *dev, u64 expires_ns)
{
	u64 now;
	s64 delta;

	if (!dev || !dev->set_next_event)
		return -EINVAL;

	now = ktime_get_mono_ns();
	delta = (s64)(expires_ns - now);

	if (delta < (s64)dev->min_delta_ns)
		delta = (s64)dev->min_delta_ns;
	if (dev->max_delta_ns && (u64)delta > dev->max_delta_ns)
		delta = (s64)dev->max_delta_ns;

	return dev->set_next_event((u64)delta, dev);
}

void clockevents_shutdown(struct clock_event_device *dev)
{
	if (!dev)
		return;

	(void)clockevents_set_mode(dev, CLOCK_EVT_MODE_SHUTDOWN);
}
