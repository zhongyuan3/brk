#include <brk/clocksource.h>
#include <brk/printk.h>
#include <brk/rtc.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/types.h>
#include <uapi/errno.h>

static u64 mono_base_cycles;
static s64 wall_to_mono_ns;
static u64 jiffies;
static SPINLOCK_DEFINE(tk_lock);
static bool initialized;

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
	u64 wall_ns = 0;

	mono_base_cycles = timer_read();

	if (rtc_is_available())
		wall_ns = rtc_read_ns();

	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	wall_to_mono_ns = (s64)wall_ns;
	jiffies = 0;
	spin_unlock_irqrestore(&tk_lock, flags);

	initialized = true;

	pr_debug("timekeeper: timekeeper initialized\n");
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
	s64 wtm;
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	wtm = wall_to_mono_ns;
	spin_unlock_irqrestore(&tk_lock, flags);

	return (ktime_t)((s64)timekeeper_mono_ns() + wtm);
}

void timekeeper_set_real(ktime_t ns)
{
	u64 mono_ns = timekeeper_mono_ns();
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	wall_to_mono_ns = (s64)ns - (s64)mono_ns;
	spin_unlock_irqrestore(&tk_lock, flags);

	if (rtc_is_available())
		rtc_set_ns((u64)ns);
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
