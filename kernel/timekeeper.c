#include <brk/cpu.h>
#include <brk/rtc.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <uapi/brk/errno.h>

static u64 mono_base_cycles;
static s64 wall_to_mono_ns;
static u64 jiffies;
static SPINLOCK_DEFINE(tk_lock);

static u64 cycles_to_ns(u64 cycles)
{
	u32 freq = cpu_get_timebase_freq();
	u64 sec = cycles / freq;
	u64 rem = cycles % freq;

	return sec * NS_PER_SEC + rem * NS_PER_SEC / freq;
}

static u64 timekeeper_mono_ns(void)
{
	return cycles_to_ns(timer_get_time() - mono_base_cycles);
}

static void ns_to_ts(s64 ns, struct timespec *ts)
{
	ts->tv_sec = ns / (s64)NS_PER_SEC;
	ts->tv_nsec = ns % (s64)NS_PER_SEC;
}

static s64 timespec_to_ns(const struct timespec *ts)
{
	return (s64)ts->tv_sec * (s64)NS_PER_SEC + ts->tv_nsec;
}

void timekeeper_init(void)
{
	struct timespec rtc_ts;
	u64 wall_ns;

	mono_base_cycles = timer_get_time();

	if (rtc_is_available()) {
		rtc_read_timespec(&rtc_ts);
		wall_ns = (u64)rtc_ts.tv_sec * NS_PER_SEC + (u64)rtc_ts.tv_nsec;
	} else {
		wall_ns = 0;
	}

	spinlock_acquire(&tk_lock);
	wall_to_mono_ns = (s64)wall_ns;
	jiffies = 0;
	spinlock_release(&tk_lock);
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
	ns_to_ts((s64)timekeeper_mono_ns(), ts);
}

void timekeeper_get_real_ts(struct timespec *ts)
{
	s64 wtm;
	s64 real_ns;

	spinlock_acquire(&tk_lock);
	wtm = wall_to_mono_ns;
	spinlock_release(&tk_lock);

	real_ns = (s64)timekeeper_mono_ns() + wtm;
	ns_to_ts(real_ns, ts);
}

void timekeeper_set_real_ts(const struct timespec *ts)
{
	u64 new_wall_ns = (u64)ts->tv_sec * NS_PER_SEC + (u64)ts->tv_nsec;
	u64 mono_ns = timekeeper_mono_ns();

	spinlock_acquire(&tk_lock);
	wall_to_mono_ns = (s64)new_wall_ns - (s64)mono_ns;
	spinlock_release(&tk_lock);

	if (rtc_is_available())
		rtc_set_timespec(ts);
}

u64 timekeeper_nanosleep(const struct timespec *dur, struct timespec *rem)
{
	s64 start_ns = (s64)timekeeper_mono_ns();
	s64 deadline_ns = start_ns + timespec_to_ns(dur);
	s64 now_ns;

	spinlock_acquire(&tk_lock);
	for (;;) {
		if (task_is_killed(current_task())) {
			s64 left_ns;

			spinlock_release(&tk_lock);
			now_ns = (s64)timekeeper_mono_ns();
			left_ns = deadline_ns - now_ns;
			if (left_ns < 0)
				left_ns = 0;
			ns_to_ts(left_ns, rem);
			return -EINTR;
		}
		now_ns = (s64)timekeeper_mono_ns();
		if (now_ns >= deadline_ns)
			break;
		task_sleep(&jiffies, &tk_lock);
	}
	spinlock_release(&tk_lock);

	rem->tv_sec = 0;
	rem->tv_nsec = 0;
	return 0;
}
