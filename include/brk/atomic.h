#ifndef _BRK_ATOMIC_H
#define _BRK_ATOMIC_H

/*
 * Kernel atomic operations over plain integer and pointer objects.
 *
 * The names mirror the compiler's __atomic builtins without the reserved
 * double-underscore prefix.  The memory order is always passed explicitly
 * using the ATOMIC_* constants below, which wrap the compiler's __ATOMIC_*
 * values:
 *
 * ATOMIC_RELAXED - atomicity only; surrounding memory accesses may be
 *                  reordered freely.
 * ATOMIC_CONSUME - orders only operations that carry a data dependency on
 *                  the loaded value; often implemented as acquire.
 * ATOMIC_ACQUIRE - no later access may be reordered before this operation;
 *                  pairs with a release to consume published data.
 * ATOMIC_RELEASE - no earlier access may be reordered after this operation;
 *                  pairs with an acquire to publish data.
 * ATOMIC_ACQ_REL - combines acquire and release; for read-modify-write
 *                  operations.
 * ATOMIC_SEQ_CST - sequentially consistent; the strongest ordering, giving
 *                  all seq_cst operations a single total order.
 */
#define ATOMIC_RELAXED __ATOMIC_RELAXED
#define ATOMIC_CONSUME __ATOMIC_CONSUME
#define ATOMIC_ACQUIRE __ATOMIC_ACQUIRE
#define ATOMIC_RELEASE __ATOMIC_RELEASE
#define ATOMIC_ACQ_REL __ATOMIC_ACQ_REL
#define ATOMIC_SEQ_CST __ATOMIC_SEQ_CST

#define atomic_load_n(p, order) __atomic_load_n((p), (order))
#define atomic_store_n(p, v, order) __atomic_store_n((p), (v), (order))
#define atomic_exchange_n(p, v, order) __atomic_exchange_n((p), (v), (order))

#define atomic_compare_exchange_n(p, expected, desired, weak, success,  \
				  failure)                              \
	__atomic_compare_exchange_n((p), (expected), (desired), (weak), \
				    (success), (failure))

#define atomic_fetch_add(p, v, order) __atomic_fetch_add((p), (v), (order))
#define atomic_fetch_sub(p, v, order) __atomic_fetch_sub((p), (v), (order))
#define atomic_fetch_or(p, v, order) __atomic_fetch_or((p), (v), (order))
#define atomic_fetch_xor(p, v, order) __atomic_fetch_xor((p), (v), (order))
#define atomic_fetch_and(p, v, order) __atomic_fetch_and((p), (v), (order))

#define atomic_add_fetch(p, v, order) __atomic_add_fetch((p), (v), (order))
#define atomic_sub_fetch(p, v, order) __atomic_sub_fetch((p), (v), (order))
#define atomic_or_fetch(p, v, order) __atomic_or_fetch((p), (v), (order))
#define atomic_xor_fetch(p, v, order) __atomic_xor_fetch((p), (v), (order))
#define atomic_and_fetch(p, v, order) __atomic_and_fetch((p), (v), (order))

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)

#endif
