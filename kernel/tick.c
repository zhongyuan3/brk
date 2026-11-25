#define pr_fmt(fmt) "tick: " fmt
#include <brk/clockevent.h>
#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/spinlock.h>
#include <brk/thread.h>
#include <brk/tick.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/timer_list.h>
#include <brk/types.h>
#include <stdbool.h>
#include <uapi/errno.h>

struct tick_cpu {
	struct list_head handlers;
	spinlock_t lock;
	u64 last_tick_ns;
	bool initialized;
};

static struct tick_cpu tick_cpus[MAX_CPUS];
static u64 tick_epoch_ns;
static bool tick_ready;

void tick_init(void)
{
	struct clock_event_device *dev = clockevents_get_current();
	int err;

	if (!dev)
		panic("tick: no clock event device\n");

	__atomic_store_n(&tick_epoch_ns, ktime_get_mono_ns(), __ATOMIC_RELAXED);
	__atomic_store_n(&tick_ready, true, __ATOMIC_RELEASE);

	err = clockevents_set_mode(dev, CLOCK_EVT_MODE_ONESHOT);
	if (err)
		pr_warn("failed to set oneshot mode: %d\n", err);

	tick_init_cpu();

	pr_debug("started (HZ=%d)\n", HZ);
}

void tick_init_cpu(void)
{
	cpuid_t cpu = this_cpuid();
	struct clock_event_device *dev;
	struct tick_cpu *tc;
	u64 now;
	int err;

	if (cpu >= MAX_CPUS)
		panic("tick: invalid cpu id %lu\n", cpu);

	tc = &tick_cpus[cpu];
	dev = clockevents_get_current();
	if (!dev)
		panic("tick: no clock event device\n");

	if (!tc->initialized) {
		list_init(&tc->handlers);
		spinlock_init(&tc->lock, "tick");
		tc->initialized = true;
	}

	dev->event_handler = tick_handle_periodic;

	now = ktime_get_mono_ns();
	tc->last_tick_ns = now;

	err = clockevents_program_event(dev, now + TICK_NS);
	if (err)
		panic("tick: failed to program first event on cpu %lu: %d\n",
		      cpu, err);
}

void tick_handle_irq(void)
{
	struct clock_event_device *dev = clockevents_get_current();

	if (!dev)
		panic("tick: no clock event device\n");

	tick_handle_periodic(dev);
}

void tick_handle_periodic(struct clock_event_device *dev)
{
	cpuid_t cpu = this_cpuid();
	struct tick_cpu *tc;
	struct tick_handler *th;
	irqstate_t flags;
	u64 now;
	u64 next;
	int err;

	if (!dev)
		return;
	if (cpu >= MAX_CPUS)
		panic("tick: invalid cpu id %lu\n", cpu);

	tc = &tick_cpus[cpu];
	if (!tc->initialized)
		panic("tick: cpu %lu has no tick device\n", cpu);

	now = ktime_get_mono_ns();

	flags = spin_lock_irqsave(&tc->lock);
	if (now > tc->last_tick_ns) {
		u64 delta = now - tc->last_tick_ns;
		u64 ticks = delta / TICK_NS;

		if (ticks)
			tc->last_tick_ns += ticks * TICK_NS;
	}
	next = tc->last_tick_ns + TICK_NS;
	spin_unlock_irqrestore(&tc->lock, flags);

	/*
	 * Software timers are global; any CPU may service expired timers.
	 * The timer list serializes callbacks internally.
	 */
	timer_run_expired(now);

	flags = spin_lock_irqsave(&tc->lock);
	list_for_each_entry(th, &tc->handlers, node) {
		if (th->fn)
			th->fn();
	}
	spin_unlock_irqrestore(&tc->lock, flags);

	err = clockevents_program_event(dev, next);
	if (err)
		panic("tick: failed to program next event on cpu %lu: %d\n",
		      cpu, err);

	sched_tick();
}

u64 tick_get_jiffies(void)
{
	u64 now;
	u64 epoch;

	if (!__atomic_load_n(&tick_ready, __ATOMIC_ACQUIRE))
		return 0;

	now = ktime_get_mono_ns();
	epoch = __atomic_load_n(&tick_epoch_ns, __ATOMIC_RELAXED);
	if (now <= epoch)
		return 0;
	return (now - epoch) / TICK_NS;
}

int tick_register_handler(struct tick_handler *th)
{
	cpuid_t cpu = this_cpuid();
	struct tick_cpu *tc;
	irqstate_t flags;

	if (!th || !th->fn || cpu >= MAX_CPUS)
		return -EINVAL;

	tc = &tick_cpus[cpu];
	flags = spin_lock_irqsave(&tc->lock);
	if (!tc->initialized) {
		spin_unlock_irqrestore(&tc->lock, flags);
		return -EAGAIN;
	}
	if (th->registered) {
		spin_unlock_irqrestore(&tc->lock, flags);
		return -EBUSY;
	}

	th->cpu = cpu;
	list_add_tail(&th->node, &tc->handlers);
	th->registered = true;
	spin_unlock_irqrestore(&tc->lock, flags);
	return 0;
}

void tick_unregister_handler(struct tick_handler *th)
{
	struct tick_cpu *tc;
	irqstate_t flags;
	cpuid_t cpu;

	if (!th || !th->registered)
		return;

	cpu = th->cpu;
	if (cpu >= MAX_CPUS)
		return;

	tc = &tick_cpus[cpu];
	flags = spin_lock_irqsave(&tc->lock);
	if (th->registered) {
		list_del_init(&th->node);
		th->registered = false;
	}
	spin_unlock_irqrestore(&tc->lock, flags);
}
