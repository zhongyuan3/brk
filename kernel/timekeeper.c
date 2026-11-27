#define pr_fmt(fmt) "timekeeper: " fmt
#include <brk/atomic.h>
#include <brk/clocksource.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/seqcount.h>
#include <brk/spinlock.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/types.h>
#include <stdbool.h>
#include <uapi/errno.h>

/*
 * Snapshot of the clock source used for CLOCK_MONOTONIC.  cycle_last is the
 * source's raw cycle value when timekeeping started; nsec is the time elapsed
 * before that point (zero for the initial source).
 */
struct tk_read_base {
	struct clocksource *clock;
	u64 cycle_last;
	u64 nsec;
};

struct timekeeper {
	struct tk_read_base tkr_mono;
	s64 offs_real;
	s64 offs_boot;
	seqcount_t seq;
	spinlock_t lock;
	bool ready;
};

static struct timekeeper tk = {
	.seq = SEQCOUNT_INITIALIZER,
	.lock = SPINLOCK_INITIALIZER("timekeeper"),
};

static bool wall_valid;
static struct wall_clock *wall_clock;

static u64 tk_read_base_ns(const struct tk_read_base *tkr)
{
	struct clocksource *cs = tkr->clock;
	u64 cycles;

	if (!cs || !cs->frequency)
		return tkr->nsec;

	cycles = cs->read();
	return tkr->nsec +
	       clocksource_cycles_to_ns(cs, cycles - tkr->cycle_last);
}

static void timekeeping_forward_now(struct timekeeper *tkp)
{
	struct tk_read_base *tkr = &tkp->tkr_mono;
	struct clocksource *cs = tkr->clock;
	u64 now;
	u64 delta;

	if (!cs || !cs->frequency)
		return;

	now = cs->read();
	delta = now - tkr->cycle_last;
	tkr->nsec += clocksource_cycles_to_ns(cs, delta);
	tkr->cycle_last = now;
}

static void timekeeping_set_real_locked(struct timekeeper *tkp, s64 ns)
{
	timekeeping_forward_now(tkp);
	tkp->offs_real = ns - (s64)tkp->tkr_mono.nsec;
}

u64 ktime_get_mono_ns(void)
{
	struct timekeeper *tkp = &tk;
	u32 seq;
	u64 ns;

	if (!atomic_load_n(&tkp->ready, ATOMIC_ACQUIRE))
		return 0;

	do {
		seq = read_seqcount_begin(&tkp->seq);
		ns = tk_read_base_ns(&tkp->tkr_mono);
	} while (read_seqcount_retry(&tkp->seq, seq));

	return ns;
}

u64 ktime_get_boottime_ns(void)
{
	struct timekeeper *tkp = &tk;
	u32 seq;
	s64 ns;

	if (!atomic_load_n(&tkp->ready, ATOMIC_ACQUIRE))
		return 0;

	do {
		seq = read_seqcount_begin(&tkp->seq);
		ns = (s64)tk_read_base_ns(&tkp->tkr_mono) + tkp->offs_boot;
	} while (read_seqcount_retry(&tkp->seq, seq));

	return (u64)ns;
}

void timekeeper_init(void)
{
	struct clocksource *cs = clocksource_get_current();
	irqstate_t flags;

	if (!cs)
		panic("timekeeper: no clocksource selected\n");

	/*
	 * Establish the monotonic epoch exactly once.  This is the equivalent
	 * of Linux's timekeeper_setup_internals(): the first read after this
	 * point is near zero, and the base is never moved by clock read paths.
	 */
	flags = spin_lock_irqsave(&tk.lock);

	if (atomic_load_n(&tk.ready, ATOMIC_RELAXED)) {
		spin_unlock_irqrestore(&tk.lock, flags);
		pr_warn("already initialized\n");
		return;
	}

	write_seqcount_begin(&tk.seq);
	tk.tkr_mono.clock = cs;
	tk.tkr_mono.cycle_last = cs->read();
	tk.tkr_mono.nsec = 0;
	tk.offs_real = 0;
	tk.offs_boot = 0;
	write_seqcount_end(&tk.seq);

	wall_valid = false;
	wall_clock = NULL;
	atomic_store_n(&tk.ready, true, ATOMIC_RELEASE);
	spin_unlock_irqrestore(&tk.lock, flags);

	pr_debug("initialized (wall clock pending)\n");
}

int timekeeper_register_wall_clock(struct wall_clock *wc)
{
	u64 wall_ns = 0;
	irqstate_t flags;
	int err;

	if (!wc || !wc->ops || !wc->ops->read_ns)
		return -EINVAL;

	err = wc->ops->read_ns(wc, &wall_ns);
	if (err)
		return err;

	flags = spin_lock_irqsave(&tk.lock);
	if (wall_clock) {
		spin_unlock_irqrestore(&tk.lock, flags);
		return -EBUSY;
	}

	if (!wall_valid) {
		/*
		 * This is the equivalent of rtc_hctosys() calling
		 * do_settimeofday64(): establish realtime from the provider
		 * without writing the same time back to the RTC.
		 */
		write_seqcount_begin(&tk.seq);
		timekeeping_set_real_locked(&tk, (s64)wall_ns);
		write_seqcount_end(&tk.seq);
		wall_valid = true;
	}
	wall_clock = wc;
	spin_unlock_irqrestore(&tk.lock, flags);

	pr_debug("wall clock %s registered\n", wc->name ? wc->name : "?");
	return 0;
}

void timekeeper_unregister_wall_clock(struct wall_clock *wc)
{
	irqstate_t flags;

	if (!wc)
		return;

	flags = spin_lock_irqsave(&tk.lock);
	if (wall_clock == wc)
		wall_clock = NULL;
	spin_unlock_irqrestore(&tk.lock, flags);
}

ktime_t timekeeper_real(void)
{
	struct timekeeper *tkp = &tk;
	u32 seq;
	s64 ns;

	if (!atomic_load_n(&tkp->ready, ATOMIC_ACQUIRE))
		return 0;

	do {
		seq = read_seqcount_begin(&tkp->seq);
		ns = (s64)tk_read_base_ns(&tkp->tkr_mono) + tkp->offs_real;
	} while (read_seqcount_retry(&tkp->seq, seq));

	return (ktime_t)ns;
}

int timekeeper_set_real(ktime_t ns)
{
	struct wall_clock *wc;
	irqstate_t flags;
	int err = 0;

	flags = spin_lock_irqsave(&tk.lock);

	write_seqcount_begin(&tk.seq);
	timekeeping_set_real_locked(&tk, (s64)ns);
	write_seqcount_end(&tk.seq);
	wall_valid = true;

	/*
	 * Keep the provider pinned by the timekeeper lock so a concurrent
	 * unregister cannot free it between the pointer load and the callback.
	 * Wall-clock callbacks must not sleep or call back into timekeeper.
	 */
	wc = wall_clock;
	if (wc && wc->ops->set_ns)
		err = wc->ops->set_ns(wc, (u64)ns);

	spin_unlock_irqrestore(&tk.lock, flags);
	return err;
}
