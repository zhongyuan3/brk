#define pr_fmt(fmt) "tick: " fmt
#include <brk/clockevent.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/tick.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/timer_list.h>
#include <brk/types.h>
#include <stdbool.h>
#include <uapi/errno.h>

static u64 jiffies;
static u64 last_tick_ns;
static LIST_DEFINE(tick_handlers);
static SPINLOCK_DEFINE(tick_lock);
static SPINLOCK_DEFINE(tick_handlers_lock);

void tick_init(void)
{
	struct clock_event_device *dev = clockevents_get_current();
	int err;

	if (!dev) {
		pr_err("no clock event device\n");
		return;
	}

	dev->event_handler = tick_handle_periodic;
	err = clockevents_set_mode(dev, CLOCK_EVT_MODE_PERIODIC);
	if (err)
		pr_warn("failed to set periodic mode: %d\n", err);

	last_tick_ns = ktime_get_mono_ns();
	jiffies = 0;

	err = clockevents_program_event(dev, last_tick_ns + TICK_NS);
	if (err)
		pr_err("failed to program first tick: %d\n", err);

	pr_debug("started (HZ=%d)\n", HZ);
}

void tick_handle_periodic(struct clock_event_device *dev)
{
	struct tick_handler *th;
	irqstate_t flags;
	u64 now;
	u64 next;

	if (!dev)
		return;

	timekeeping_advance();
	now = ktime_get_mono_ns();

	flags = spin_lock_irqsave(&tick_lock);
	if (now > last_tick_ns) {
		u64 delta = now - last_tick_ns;
		u64 ticks = delta / TICK_NS;

		if (ticks) {
			last_tick_ns += ticks * TICK_NS;
			jiffies += ticks;
		}
	}
	next = last_tick_ns + TICK_NS;
	spin_unlock_irqrestore(&tick_lock, flags);

	timer_run_expired(now);

	flags = spin_lock_irqsave(&tick_handlers_lock);
	list_for_each_entry(th, &tick_handlers, node) {
		if (th->fn)
			th->fn();
	}
	spin_unlock_irqrestore(&tick_handlers_lock, flags);

	(void)clockevents_program_event(dev, next);
}

u64 tick_get_jiffies(void)
{
	u64 j;
	irqstate_t flags = spin_lock_irqsave(&tick_lock);

	j = jiffies;
	spin_unlock_irqrestore(&tick_lock, flags);
	return j;
}

int tick_register_handler(struct tick_handler *th)
{
	irqstate_t flags;

	if (!th || !th->fn)
		return -EINVAL;

	flags = spin_lock_irqsave(&tick_handlers_lock);
	if (th->registered) {
		spin_unlock_irqrestore(&tick_handlers_lock, flags);
		return -EBUSY;
	}
	list_add_tail(&th->node, &tick_handlers);
	th->registered = true;
	spin_unlock_irqrestore(&tick_handlers_lock, flags);
	return 0;
}

void tick_unregister_handler(struct tick_handler *th)
{
	irqstate_t flags;

	if (!th)
		return;

	flags = spin_lock_irqsave(&tick_handlers_lock);
	if (th->registered) {
		list_del_init(&th->node);
		th->registered = false;
	}
	spin_unlock_irqrestore(&tick_handlers_lock, flags);
}
