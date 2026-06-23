#include <brk/cpu.h>
#include <brk/rtc.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <uapi/brk/errno.h>

static uint64_t mono_base_cycles;
static int64_t wall_to_mono_ns;
static uint64_t jiffies;
static SPINLOCK_DEFINE(tk_lock);

static uint64_t cycles_to_ns(uint64_t cycles)
{
	uint32_t freq = cpu_get_timebase_freq();
	uint64_t sec = cycles / freq;
	uint64_t rem = cycles % freq;

	return sec * NS_PER_SEC + rem * NS_PER_SEC / freq;
}

static uint64_t timekeeper_mono_ns(void)
{
	return cycles_to_ns(timer_get_time() - mono_base_cycles);
}

static void ns_to_ts(int64_t ns, struct timespec *ts)
{
	ts->tv_sec = ns / (int64_t)NS_PER_SEC;
	ts->tv_nsec = ns % (int64_t)NS_PER_SEC;
}

static int64_t timespec_to_ns(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * (int64_t)NS_PER_SEC + ts->tv_nsec;
}

void timekeeper_init(void)
{
	struct timespec rtc_ts;
	uint64_t wall_ns;

	mono_base_cycles = timer_get_time();

	if (rtc_is_available()) {
		rtc_read_timespec(&rtc_ts);
		wall_ns = (uint64_t)rtc_ts.tv_sec * NS_PER_SEC +
			  (uint64_t)rtc_ts.tv_nsec;
	} else {
		wall_ns = 0;
	}

	spinlock_acquire(&tk_lock);
	wall_to_mono_ns = (int64_t)wall_ns;
	jiffies = 0;
	spinlock_release(&tk_lock);
}

void timekeeper_tick(void)
{
	spinlock_acquire(&tk_lock);
	++jiffies;
	spinlock_release(&tk_lock);
}

uint64_t jiffies_get(void)
{
	uint64_t j;

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
	ns_to_ts((int64_t)timekeeper_mono_ns(), ts);
}

void timekeeper_get_real_ts(struct timespec *ts)
{
	int64_t wtm;
	int64_t real_ns;

	spinlock_acquire(&tk_lock);
	wtm = wall_to_mono_ns;
	spinlock_release(&tk_lock);

	real_ns = (int64_t)timekeeper_mono_ns() + wtm;
	ns_to_ts(real_ns, ts);
}

void timekeeper_set_real_ts(const struct timespec *ts)
{
	uint64_t new_wall_ns =
		(uint64_t)ts->tv_sec * NS_PER_SEC + (uint64_t)ts->tv_nsec;
	uint64_t mono_ns = timekeeper_mono_ns();

	spinlock_acquire(&tk_lock);
	wall_to_mono_ns = (int64_t)new_wall_ns - (int64_t)mono_ns;
	spinlock_release(&tk_lock);

	if (rtc_is_available())
		rtc_set_timespec(ts);
}

uint64_t timekeeper_nanosleep(const struct timespec *dur, struct timespec *rem)
{
	int64_t start_ns = (int64_t)timekeeper_mono_ns();
	int64_t deadline_ns = start_ns + timespec_to_ns(dur);
	int64_t now_ns;

	spinlock_acquire(&tk_lock);
	for (;;) {
		if (task_is_killed(current_task())) {
			int64_t left_ns;

			spinlock_release(&tk_lock);
			now_ns = (int64_t)timekeeper_mono_ns();
			left_ns = deadline_ns - now_ns;
			if (left_ns < 0)
				left_ns = 0;
			ns_to_ts(left_ns, rem);
			return -EINTR;
		}
		now_ns = (int64_t)timekeeper_mono_ns();
		if (now_ns >= deadline_ns)
			break;
		task_sleep(&jiffies, &tk_lock);
	}
	spinlock_release(&tk_lock);

	rem->tv_sec = 0;
	rem->tv_nsec = 0;
	return 0;
}
