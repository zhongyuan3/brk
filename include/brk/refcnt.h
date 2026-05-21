#ifndef BRK_REFCNT_H
#define BRK_REFCNT_H

#include <brk/lock.h>

typedef struct __refcnt {
	int counter;
	spinlock_t lock;
} refcnt_t;

void refcnt_init(refcnt_t *rc, int val);
int refcnt_read(refcnt_t *rc);
int refcnt_fetch_inc(refcnt_t *rc);
int refcnt_fetch_dec(refcnt_t *rc);
int refcnt_inc_fetch(refcnt_t *rc);
int refcnt_dec_fetch(refcnt_t *rc);
void refcnt_inc(refcnt_t *rc);
void refcnt_dec(refcnt_t *rc);

#endif
