#ifndef BRK_REFCNT_H
#define BRK_REFCNT_H

#include <brk/refcnt_types.h>

void refcnt_init(refcnt_t *rc, refcnt_value_t val);
refcnt_value_t refcnt_read(refcnt_t *rc);
refcnt_value_t refcnt_fetch_inc(refcnt_t *rc);
refcnt_value_t refcnt_fetch_dec(refcnt_t *rc);
refcnt_value_t refcnt_inc_fetch(refcnt_t *rc);
refcnt_value_t refcnt_dec_fetch(refcnt_t *rc);
void refcnt_inc(refcnt_t *rc);
void refcnt_dec(refcnt_t *rc);

#endif
