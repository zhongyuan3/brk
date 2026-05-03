#ifndef BRK_REFCNT_H
#define BRK_REFCNT_H

#include <brk/lock.h>

typedef struct {
	int counter;
	spinlock_t lock;
} arc_t;

/**
 * Initialize an arc_t structure.
 *
 * @param arc The arc_t structure to initialize.
 * @param v The initial value of the counter.
 */
static inline void arc_init(arc_t *arc, int v)
{
	spinlock_init(&arc->lock, "arc");
	arc->counter = v;
}

/**
 * Get the current value of the counter.
 *
 * @param arc The arc_t structure.
 * @return The current value of the counter.
 */
static inline int arc_get(arc_t *arc)
{
	spinlock_acquire(&arc->lock);
	int c = arc->counter;
	spinlock_release(&arc->lock);
	return c;
}

/**
 * Increment the counter and return the old value.
 *
 * @param arc The arc_t structure.
 * @return The old value of the counter.
 */
static inline int arc_fetch_inc(arc_t *arc)
{
	spinlock_acquire(&arc->lock);
	int old = arc->counter++;
	spinlock_release(&arc->lock);
	return old;
}

/**
 * Decrement the counter and return the old value.
 *
 * @param arc The arc_t structure.
 * @return The old value of the counter.
 */
static inline int arc_fetch_dec(arc_t *arc)
{
	spinlock_acquire(&arc->lock);
	int old = arc->counter--;
	spinlock_release(&arc->lock);
	return old;
}

/**
 * Increment the counter and return the new value.
 *
 * @param arc The arc_t structure.
 * @return The new value of the counter.
 */
static inline int arc_inc_fetch(arc_t *arc)
{
	spinlock_acquire(&arc->lock);
	int new = ++arc->counter;
	spinlock_release(&arc->lock);
	return new;
}

/**
 * Decrement the counter and return the new value.
 *
 * @param arc The arc_t structure.
 * @return The new value of the counter.
 */
static inline int arc_dec_fetch(arc_t *arc)
{
	spinlock_acquire(&arc->lock);
	int new = --arc->counter;
	spinlock_release(&arc->lock);
	return new;
}

/**
 * Increment the counter.
 *
 * @param arc The arc_t structure.
 */
static inline void arc_inc(arc_t *arc)
{
	arc_inc_fetch(arc);
}

/**
 * Decrement the counter.
 *
 * @param arc The arc_t structure.
 */
static inline void arc_dec(arc_t *arc)
{
	arc_dec_fetch(arc);
}

#endif
