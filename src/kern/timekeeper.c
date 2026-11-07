#include <brk/clocksource.h>
#include <brk/initcall.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/types.h>
#include <uapi/errno.h>

static u64 mono_base_ns;
static s64 wall_offset_ns;
static u64 jiffies;
static SPINLOCK_DEFINE(tk_lock);
static bool initialized;
static bool wall_valid;
static struct wall_clock *wall_clock;
static LIST_DEFINE(tick_handlers);
static SPINLOCK_DEFINE(tick_lock);

bool timekeeper_initialized(void)
{
	return initialized;
}

u64 timekeeper_mono_ns(void)
{
	return clocksource_read_ns() - mono_base_ns;
}

void timekeeper_init(void)
{
	mono_base_ns = clocksource_read_ns();

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

int timekeeper_register_tick_handler(struct tick_handler *th)
{
	irqstate_t flags;

	if (!th || !th->fn)
		return -EINVAL;

	flags = spin_lock_irqsave(&tick_lock);
	if (th->registered) {
		spin_unlock_irqrestore(&tick_lock, flags);
		return -EBUSY;
	}
	list_add_tail(&th->node, &tick_handlers);
	th->registered = true;
	spin_unlock_irqrestore(&tick_lock, flags);
	return 0;
}

void timekeeper_unregister_tick_handler(struct tick_handler *th)
{
	irqstate_t flags;

	if (!th)
		return;

	flags = spin_lock_irqsave(&tick_lock);
	if (th->registered) {
		list_del_init(&th->node);
		th->registered = false;
	}
	spin_unlock_irqrestore(&tick_lock, flags);
}

void timekeeper_tick(void)
{
	struct tick_handler *th;
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	++jiffies;
	spin_unlock_irqrestore(&tk_lock, flags);

	flags = spin_lock_irqsave(&tick_lock);
	list_for_each_entry(th, &tick_handlers, node) {
		if (th->fn)
			th->fn();
	}
	spin_unlock_irqrestore(&tick_lock, flags);

	task_wake_all(&jiffies);
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

	/* CLOCK_REALTIME = monotonic time + wall offset. */
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

#ifndef NDEBUG
struct timekeeper_test_state {
	struct wall_clock *wall_clock;
	bool wall_valid;
	s64 wall_offset_ns;
};

static unsigned int timekeeper_test_failures;
static u64 timekeeper_mock_wall_ns;
static unsigned int timekeeper_mock_reads;
static unsigned int timekeeper_mock_sets;

#define TIMEKEEPER_CHECK(cond, name)                                      \
	do {                                                              \
		if (!(cond)) {                                            \
			pr_err("timekeeper-selftest: %s failed\n", name); \
			timekeeper_test_failures++;                       \
		}                                                         \
	} while (0)

static u64 timekeeper_test_abs_diff(u64 a, u64 b)
{
	return a > b ? a - b : b - a;
}

static int timekeeper_mock_read(struct wall_clock *wc, u64 *ns)
{
	(void)wc;

	timekeeper_mock_reads++;
	*ns = timekeeper_mock_wall_ns;
	return 0;
}

static int timekeeper_mock_set(struct wall_clock *wc, u64 ns)
{
	(void)wc;

	timekeeper_mock_sets++;
	timekeeper_mock_wall_ns = ns;
	return 0;
}

static const struct wall_clock_ops timekeeper_mock_ops = {
	.read_ns = timekeeper_mock_read,
	.set_ns = timekeeper_mock_set,
};

static struct wall_clock timekeeper_mock = {
	.name = "timekeeper-selftest",
	.ops = &timekeeper_mock_ops,
};

static void timekeeper_test_save(struct timekeeper_test_state *saved)
{
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	saved->wall_clock = wall_clock;
	saved->wall_valid = wall_valid;
	saved->wall_offset_ns = wall_offset_ns;
	spin_unlock_irqrestore(&tk_lock, flags);
}

static void timekeeper_test_clear(void)
{
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	wall_clock = NULL;
	wall_valid = false;
	wall_offset_ns = 0;
	spin_unlock_irqrestore(&tk_lock, flags);
}

static void timekeeper_test_restore(const struct timekeeper_test_state *saved)
{
	irqstate_t flags = spin_lock_irqsave(&tk_lock);

	wall_clock = saved->wall_clock;
	wall_valid = saved->wall_valid;
	wall_offset_ns = saved->wall_offset_ns;
	spin_unlock_irqrestore(&tk_lock, flags);
}

static void timekeeper_selftest_init(void)
{
	struct timekeeper_test_state saved;
	u64 before, after, expected;
	u64 set_ns;
	int err;

	timekeeper_test_save(&saved);

	before = timekeeper_mono_ns();
	for (volatile int i = 0; i < 100000; i++)
		;
	after = timekeeper_mono_ns();
	TIMEKEEPER_CHECK(after >= before, "monotonic nondecreasing");

	/* Provider registration applies its wall time. */
	timekeeper_test_clear();
	timekeeper_mock_wall_ns = 1700000000ULL * NS_PER_SEC;
	timekeeper_mock_reads = 0;
	timekeeper_mock_sets = 0;

	err = timekeeper_register_wall_clock(&timekeeper_mock);
	TIMEKEEPER_CHECK(err == 0, "provider register");
	TIMEKEEPER_CHECK(timekeeper_mock_reads == 1, "provider read");
	TIMEKEEPER_CHECK(timekeeper_test_abs_diff((u64)timekeeper_real(),
						  timekeeper_mock_wall_ns) <
				 50 * NS_PER_MS,
			 "wall time applied");

	set_ns = timekeeper_mock_wall_ns + 5 * NS_PER_SEC;
	err = timekeeper_set_real((ktime_t)set_ns);
	TIMEKEEPER_CHECK(err == 0, "set_real with provider");
	TIMEKEEPER_CHECK(timekeeper_mock_sets == 1, "provider set");
	TIMEKEEPER_CHECK(timekeeper_real() >= (ktime_t)(set_ns - NS_PER_SEC),
			 "set_real applied");

	timekeeper_unregister_wall_clock(&timekeeper_mock);

	/* User-set wall time must not be overwritten by a later provider. */
	timekeeper_test_clear();
	expected = 1800000000ULL * NS_PER_SEC;
	err = timekeeper_set_real((ktime_t)expected);
	TIMEKEEPER_CHECK(err == 0, "set_real without provider");

	timekeeper_mock_wall_ns = 1600000000ULL * NS_PER_SEC;
	timekeeper_mock_reads = 0;
	timekeeper_mock_sets = 0;
	err = timekeeper_register_wall_clock(&timekeeper_mock);
	TIMEKEEPER_CHECK(err == 0, "late provider register");
	TIMEKEEPER_CHECK(timekeeper_test_abs_diff((u64)timekeeper_real(),
						  expected) < 50 * NS_PER_MS,
			 "user time preserved");
	timekeeper_unregister_wall_clock(&timekeeper_mock);

	timekeeper_test_restore(&saved);

	if (timekeeper_test_failures)
		pr_err("timekeeper-selftest: %u failure(s)\n",
		       timekeeper_test_failures);
	else
		pr_info("timekeeper-selftest: all tests passed\n");
}
late_initcall(timekeeper_selftest_init);
#endif
