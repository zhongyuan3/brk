#include <brk/clocksource.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/types.h>
#include <uapi/errno.h>

static u64 mono_base_cycles;
static s64 wall_offset_ns;
static u64 jiffies;
static SPINLOCK_DEFINE(tk_lock);
static bool initialized;
static bool wall_valid;
static struct wall_clock *wall_clock;

bool timekeeper_initialized(void)
{
	return initialized;
}

static u64 cycles_to_ns(u64 cycles)
{
	u32 freq = clocksource_get_timebase_freq();
	u64 sec = cycles / freq;
	u64 rem = cycles % freq;

	return sec * NS_PER_SEC + rem * NS_PER_SEC / freq;
}

u64 timekeeper_mono_ns(void)
{
	return cycles_to_ns(timer_read() - mono_base_cycles);
}

void timekeeper_init(void)
{
	mono_base_cycles = timer_read();

	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	wall_offset_ns = 0;
	wall_valid = false;
	wall_clock = NULL;
	jiffies = 0;
	spin_unlock_irqrestore(&tk_lock, flags);

	initialized = true;

	pr_debug("timekeeper: initialized (wall clock pending)\n");
}

int timekeeper_register_wall_clock(struct wall_clock *wc)
{
	u64 wall_ns = 0;
	u64 mono_ns;
	irqstate_t flags;
	int err;

	if (!wc || !wc->ops || !wc->ops->read_ns)
		return -EINVAL;
	if (!initialized)
		return -EAGAIN;

	err = wc->ops->read_ns(wc, &wall_ns);
	if (err)
		return err;

	mono_ns = timekeeper_mono_ns();

	flags = spin_lock_irqsave(&tk_lock);
	if (wall_clock) {
		spin_unlock_irqrestore(&tk_lock, flags);
		return -EBUSY;
	}

	wall_clock = wc;
	if (!wall_valid) {
		wall_offset_ns = (s64)wall_ns - (s64)mono_ns;
		wall_valid = true;
	}
	spin_unlock_irqrestore(&tk_lock, flags);

	pr_debug("timekeeper: wall clock %s registered\n",
		 wc->name ? wc->name : "?");
	return 0;
}

void timekeeper_unregister_wall_clock(struct wall_clock *wc)
{
	irqstate_t flags;

	if (!wc)
		return;

	flags = spin_lock_irqsave(&tk_lock);
	if (wall_clock == wc)
		wall_clock = NULL;
	spin_unlock_irqrestore(&tk_lock, flags);
}

void timekeeper_tick(void)
{
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	++jiffies;
	spin_unlock_irqrestore(&tk_lock, flags);
}

u64 jiffies_get(void)
{
	u64 j;
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	j = jiffies;
	spin_unlock_irqrestore(&tk_lock, flags);
	return j;
}

void *timekeeper_wait_chan(void)
{
	return &jiffies;
}

ktime_t timekeeper_real(void)
{
	s64 offset;
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	offset = wall_offset_ns;
	spin_unlock_irqrestore(&tk_lock, flags);

	return (ktime_t)((s64)timekeeper_mono_ns() + offset);
}

int timekeeper_set_real(ktime_t ns)
{
	u64 mono_ns = timekeeper_mono_ns();
	struct wall_clock *wc;
	irqstate_t flags;

	flags = spin_lock_irqsave(&tk_lock);
	wall_offset_ns = (s64)ns - (s64)mono_ns;
	wall_valid = true;
	wc = wall_clock;
	spin_unlock_irqrestore(&tk_lock, flags);

	if (wc && wc->ops->set_ns)
		return wc->ops->set_ns(wc, (u64)ns);
	return 0;
}

int timekeeper_nanosleep(ktime_t dur, ktime_t *rem)
{
	s64 start_ns = (s64)timekeeper_mono_ns();
	s64 deadline_ns = start_ns + dur;
	s64 now_ns;
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	for (;;) {
		if (task_is_killed(current_task())) {
			s64 left_ns;

			spin_unlock_irqrestore(&tk_lock, flags);
			now_ns = (s64)timekeeper_mono_ns();
			left_ns = deadline_ns - now_ns;
			if (left_ns < 0)
				left_ns = 0;
			if (rem)
				*rem = (ktime_t)left_ns;
			return -EINTR;
		}
		now_ns = (s64)timekeeper_mono_ns();
		if (now_ns >= deadline_ns)
			break;
		task_sleep(&jiffies, &tk_lock);
	}
	spin_unlock_irqrestore(&tk_lock, flags);

	if (rem)
		*rem = 0;
	return 0;
}
