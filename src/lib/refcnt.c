#include <brk/refcnt.h>
#include <brk/spinlock.h>

void refcnt_init(refcnt_t *rc, refcnt_value_t val)
{
	spinlock_init(&rc->lock, "refcnt");
	rc->counter = val;
}

refcnt_value_t refcnt_read(refcnt_t *rc)
{
	irqstate_t flags = spin_lock_irqsave(&rc->lock);
	refcnt_value_t val = rc->counter;
	spin_unlock_irqrestore(&rc->lock, flags);
	return val;
}

refcnt_value_t refcnt_fetch_inc(refcnt_t *rc)
{
	irqstate_t flags = spin_lock_irqsave(&rc->lock);
	refcnt_value_t old_val = rc->counter++;
	spin_unlock_irqrestore(&rc->lock, flags);
	return old_val;
}

refcnt_value_t refcnt_fetch_dec(refcnt_t *rc)
{
	irqstate_t flags = spin_lock_irqsave(&rc->lock);
	refcnt_value_t old_val = rc->counter > 0 ? rc->counter-- : 0;
	spin_unlock_irqrestore(&rc->lock, flags);
	return old_val;
}

refcnt_value_t refcnt_inc_fetch(refcnt_t *rc)
{
	irqstate_t flags = spin_lock_irqsave(&rc->lock);
	refcnt_value_t new_val = ++rc->counter;
	spin_unlock_irqrestore(&rc->lock, flags);
	return new_val;
}

refcnt_value_t refcnt_dec_fetch(refcnt_t *rc)
{
	irqstate_t flags = spin_lock_irqsave(&rc->lock);
	refcnt_value_t new_val = rc->counter > 0 ? --rc->counter : 0;
	spin_unlock_irqrestore(&rc->lock, flags);
	return new_val;
}

void refcnt_inc(refcnt_t *rc)
{
	refcnt_inc_fetch(rc);
}

void refcnt_dec(refcnt_t *rc)
{
	refcnt_dec_fetch(rc);
}

bool refcnt_inc_unless_zero(refcnt_t *rc)
{
	bool taken;

	irqstate_t flags = spin_lock_irqsave(&rc->lock);
	taken = rc->counter > 0;
	if (taken)
		rc->counter++;
	spin_unlock_irqrestore(&rc->lock, flags);
	return taken;
}
