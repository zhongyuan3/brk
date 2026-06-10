#ifndef BRK_REFCNT_H
#define BRK_REFCNT_H

#include <brk/kernel/refcnt_types.h>

void refcnt_init(refcnt_t *rc, refcnt_value_t val);
refcnt_value_t refcnt_read(refcnt_t *rc);
refcnt_value_t refcnt_fetch_inc(refcnt_t *rc);
refcnt_value_t refcnt_fetch_dec(refcnt_t *rc);
refcnt_value_t refcnt_inc_fetch(refcnt_t *rc);
refcnt_value_t refcnt_dec_fetch(refcnt_t *rc);
void refcnt_inc(refcnt_t *rc);
void refcnt_dec(refcnt_t *rc);

/*
 * refcnt_inc_unless_zero() - take a reference only if one is already held.
 *
 * Atomically increments the count unless it has already dropped to zero.
 * This lets a weak holder (e.g. a lookup table) safely upgrade to a strong
 * reference without resurrecting an object whose last reference is being
 * dropped concurrently.
 *
 * Return: true if a reference was taken, false if the count was zero.
 */
bool refcnt_inc_unless_zero(refcnt_t *rc);

#endif
