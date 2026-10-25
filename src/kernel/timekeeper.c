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
static i64 wall_to_mono_ns;
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

static void ns_to_ts(i64 ns, struct timespec *ts)
{
	ts->tv_sec = ns / (i64)NS_PER_SEC;
	ts->tv_nsec = ns % (i64)NS_PER_SEC;
}

static i64 timespec_to_ns(const struct timespec *ts)
{
	return (i64)ts->tv_sec * (i64)NS_PER_SEC + ts->tv_nsec;
}

void timekeeper_init(void)
{
	struct timespec rtc_ts;
	u64 wall_ns;

	mono_base_cycles = timer_read();

	if (rtc_is_available()) {
		rtc_read_timespec(&rtc_ts);
		wall_ns = (u64)rtc_ts.tv_sec * NS_PER_SEC + (u64)rtc_ts.tv_nsec;
	} else {
		wall_ns = 0;
	}

	spinlock_acquire(&tk_lock);
	wall_to_mono_ns = (i64)wall_ns;
	jiffies = 0;
	spinlock_release(&tk_lock);

	initialized = true;

	klog_debug("timekeeper: timekeeper initialized\n");
}

void timekeeper_tick(void)
{
	spinlock_acquire(&tk_lock);
	++jiffies;
	spinlock_release(&tk_lock);
}

u64 jiffies_get(void)
{
	u64 j;

	spinlock_acquire(&tk_lock);
	j = jiffies;
	spinlock_release(&tk_lock);
	return j;
}

void *timekeeper_wait_chan(void)
{
	return &jiffies;
}

void timekeeper_get_mono_ts(struct timespec *ts)
{
	ns_to_ts((i64)timekeeper_mono_ns(), ts);
}

void timekeeper_get_real_ts(struct timespec *ts)
{
	i64 wtm;
	i64 real_ns;

	spinlock_acquire(&tk_lock);
	wtm = wall_to_mono_ns;
	spinlock_release(&tk_lock);

	real_ns = (i64)timekeeper_mono_ns() + wtm;
	ns_to_ts(real_ns, ts);
}

void timekeeper_set_real_ts(const struct timespec *ts)
{
	u64 new_wall_ns = (u64)ts->tv_sec * NS_PER_SEC + (u64)ts->tv_nsec;
	u64 mono_ns = timekeeper_mono_ns();

	spinlock_acquire(&tk_lock);
	wall_to_mono_ns = (i64)new_wall_ns - (i64)mono_ns;
	spinlock_release(&tk_lock);

	if (rtc_is_available())
		rtc_set_timespec(ts);
}

u64 timekeeper_nanosleep(const struct timespec *dur, struct timespec *rem)
{
	i64 start_ns = (i64)timekeeper_mono_ns();
	i64 deadline_ns = start_ns + timespec_to_ns(dur);
	i64 now_ns;

	spinlock_acquire(&tk_lock);
	for (;;) {
		if (task_is_killed(current_task())) {
			i64 left_ns;

			spinlock_release(&tk_lock);
			now_ns = (i64)timekeeper_mono_ns();
			left_ns = deadline_ns - now_ns;
			if (left_ns < 0)
				left_ns = 0;
			ns_to_ts(left_ns, rem);
			return -EINTR;
		}
		now_ns = (i64)timekeeper_mono_ns();
		if (now_ns >= deadline_ns)
			break;
		task_sleep(&jiffies, &tk_lock);
	}
	spinlock_release(&tk_lock);

	rem->tv_sec = 0;
	rem->tv_nsec = 0;
	return 0;
}
