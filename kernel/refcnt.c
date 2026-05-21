#include <brk/refcnt.h>

void refcnt_init(refcnt_t *rc, int val)
{
	spinlock_init(&rc->lock, "refcnt");
	rc->counter = val;
}

int refcnt_read(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	int c = rc->counter;
	spinlock_release(&rc->lock);
	return c;
}

int refcnt_fetch_inc(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	int old = rc->counter++;
	spinlock_release(&rc->lock);
	return old;
}

int refcnt_fetch_dec(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	int old = rc->counter--;
	spinlock_release(&rc->lock);
	return old;
}

int refcnt_inc_fetch(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	int new = ++rc->counter;
	spinlock_release(&rc->lock);
	return new;
}

int refcnt_dec_fetch(refcnt_t *rc)
{
	spinlock_acquire(&rc->lock);
	int new = --rc->counter;
	spinlock_release(&rc->lock);
	return new;
}

void refcnt_inc(refcnt_t *rc)
{
	refcnt_inc_fetch(rc);
}

void refcnt_dec(refcnt_t *rc)
{
	refcnt_dec_fetch(rc);
}
