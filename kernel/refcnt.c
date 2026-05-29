#include <brk/refcnt.h>
#include <brk/spinlock.h>

void refcnt_init(refcnt_t *rc, refcnt_value_t val)
{
	spinlock_init(&rc->lock, "refcnt");
	rc->counter = val;
}

refcnt_value_t refcnt_read(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	refcnt_value_t val = rc->counter;
	spinlock_release(&rc->lock);
	return val;
}

refcnt_value_t refcnt_fetch_inc(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	refcnt_value_t old_val = rc->counter++;
	spinlock_release(&rc->lock);
	return old_val;
}

refcnt_value_t refcnt_fetch_dec(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	refcnt_value_t old_val = rc->counter > 0 ? rc->counter-- : 0;
	spinlock_release(&rc->lock);
	return old_val;
}

refcnt_value_t refcnt_inc_fetch(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	refcnt_value_t new_val = ++rc->counter;
	spinlock_release(&rc->lock);
	return new_val;
}

refcnt_value_t refcnt_dec_fetch(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	refcnt_value_t new_val = rc->counter > 0 ? --rc->counter : 0;
	spinlock_release(&rc->lock);
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
