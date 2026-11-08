#include <brk/spinlock.h>
#include <brk/timer_list.h>
#include <brk/types.h>
#include <uapi/errno.h>

static LIST_DEFINE(timer_head);
static SPINLOCK_DEFINE(timer_lock);

void timer_setup(struct timer_list *timer,
		 void (*function)(struct timer_list *timer))
{
	if (!timer)
		return;

	list_init(&timer->node);
	timer->expires_ns = 0;
	timer->function = function;
	timer->pending = false;
	timer->running = false;
}

void timer_set_expires(struct timer_list *timer, u64 expires_ns)
{
	if (timer)
		timer->expires_ns = expires_ns;
}

int timer_add(struct timer_list *timer)
{
	struct list_head *head = &timer_head;
	struct timer_list *pos;
	irqstate_t flags;

	if (!timer || !timer->function)
		return -EINVAL;

	flags = spin_lock_irqsave(&timer_lock);
	if (timer->pending) {
		spin_unlock_irqrestore(&timer_lock, flags);
		return -EALREADY;
	}

	list_for_each_entry(pos, &timer_head, node) {
		if (timer->expires_ns < pos->expires_ns) {
			head = &pos->node;
			break;
		}
	}
	list_add_tail(&timer->node, head);
	timer->pending = true;
	spin_unlock_irqrestore(&timer_lock, flags);
	return 0;
}

bool timer_del(struct timer_list *timer)
{
	irqstate_t flags;

	if (!timer)
		return false;

	flags = spin_lock_irqsave(&timer_lock);
	if (!timer->pending) {
		spin_unlock_irqrestore(&timer_lock, flags);
		return false;
	}

	list_del_init(&timer->node);
	timer->pending = false;
	spin_unlock_irqrestore(&timer_lock, flags);
	return true;
}

void timer_del_sync(struct timer_list *timer)
{
	if (!timer)
		return;

	(void)timer_del(timer);

	while (__atomic_load_n(&timer->running, __ATOMIC_ACQUIRE))
		__asm__ volatile("" ::: "memory");
}

bool timer_pending(struct timer_list *timer)
{
	bool pending;
	irqstate_t flags;

	if (!timer)
		return false;

	flags = spin_lock_irqsave(&timer_lock);
	pending = timer->pending;
	spin_unlock_irqrestore(&timer_lock, flags);
	return pending;
}

void timer_run_expired(u64 now_ns)
{
	for (;;) {
		struct timer_list *timer;
		irqstate_t flags;

		flags = spin_lock_irqsave(&timer_lock);
		if (list_empty(&timer_head)) {
			spin_unlock_irqrestore(&timer_lock, flags);
			break;
		}

		timer = list_first_entry(&timer_head, struct timer_list, node);
		if (timer->expires_ns > now_ns) {
			spin_unlock_irqrestore(&timer_lock, flags);
			break;
		}

		list_del_init(&timer->node);
		timer->pending = false;
		__atomic_store_n(&timer->running, true, __ATOMIC_RELEASE);
		spin_unlock_irqrestore(&timer_lock, flags);

		timer->function(timer);

		__atomic_store_n(&timer->running, false, __ATOMIC_RELEASE);
	}
}
