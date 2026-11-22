#ifndef STDATOMIC_H
#define STDATOMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	memory_order_relaxed = __ATOMIC_RELAXED,
	memory_order_consume = __ATOMIC_CONSUME,
	memory_order_acquire = __ATOMIC_ACQUIRE,
	memory_order_release = __ATOMIC_RELEASE,
	memory_order_acq_rel = __ATOMIC_ACQ_REL,
	memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

#define __STDATOMIC_DECLARE(type, name) \
	typedef struct {                \
		volatile type __val;    \
	} name;

__STDATOMIC_DECLARE(bool, atomic_bool)
__STDATOMIC_DECLARE(char, atomic_char)
__STDATOMIC_DECLARE(signed char, atomic_schar)
__STDATOMIC_DECLARE(unsigned char, atomic_uchar)
__STDATOMIC_DECLARE(short, atomic_short)
__STDATOMIC_DECLARE(unsigned short, atomic_ushort)
__STDATOMIC_DECLARE(int, atomic_int)
__STDATOMIC_DECLARE(unsigned int, atomic_uint)
__STDATOMIC_DECLARE(long, atomic_long)
__STDATOMIC_DECLARE(unsigned long, atomic_ulong)
__STDATOMIC_DECLARE(long long, atomic_llong)
__STDATOMIC_DECLARE(unsigned long long, atomic_ullong)
__STDATOMIC_DECLARE(intptr_t, atomic_intptr_t)
__STDATOMIC_DECLARE(uintptr_t, atomic_uintptr_t)
__STDATOMIC_DECLARE(size_t, atomic_size_t)
__STDATOMIC_DECLARE(ptrdiff_t, atomic_ptrdiff_t)
__STDATOMIC_DECLARE(intmax_t, atomic_intmax_t)
__STDATOMIC_DECLARE(uintmax_t, atomic_uintmax_t)

#undef __STDATOMIC_DECLARE

typedef struct {
	volatile bool __val;
} atomic_flag;

#define ATOMIC_VAR_INIT(value) { .__val = (value) }
#define ATOMIC_FLAG_INIT { .__val = 0 }

#define atomic_init(obj, value) ((obj)->__val = (value))

#define atomic_is_lock_free(obj) \
	__atomic_is_lock_free(sizeof((obj)->__val), (obj))

#define atomic_load_explicit(obj, order) __atomic_load_n(&(obj)->__val, (order))
#define atomic_load(obj) __atomic_load_n(&(obj)->__val, __ATOMIC_SEQ_CST)

#define atomic_store_explicit(obj, value, order) \
	__atomic_store_n(&(obj)->__val, (value), (order))
#define atomic_store(obj, value) \
	__atomic_store_n(&(obj)->__val, (value), __ATOMIC_SEQ_CST)

#define atomic_exchange_explicit(obj, value, order) \
	__atomic_exchange_n(&(obj)->__val, (value), (order))
#define atomic_exchange(obj, value) \
	__atomic_exchange_n(&(obj)->__val, (value), __ATOMIC_SEQ_CST)

#define atomic_compare_exchange_strong_explicit(obj, expected, desired,      \
						success, failure)            \
	__atomic_compare_exchange_n(&(obj)->__val, (expected), (desired), 0, \
				    (success), (failure))

#define atomic_compare_exchange_weak_explicit(obj, expected, desired, success, \
					      failure)                         \
	__atomic_compare_exchange_n(&(obj)->__val, (expected), (desired), 1,   \
				    (success), (failure))

#define atomic_compare_exchange_strong(obj, expected, desired)               \
	__atomic_compare_exchange_n(&(obj)->__val, (expected), (desired), 0, \
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define atomic_compare_exchange_weak(obj, expected, desired)                 \
	__atomic_compare_exchange_n(&(obj)->__val, (expected), (desired), 1, \
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define atomic_fetch_add_explicit(obj, arg, order) \
	__atomic_fetch_add(&(obj)->__val, (arg), (order))
#define atomic_fetch_add(obj, arg) \
	__atomic_fetch_add(&(obj)->__val, (arg), __ATOMIC_SEQ_CST)

#define atomic_fetch_sub_explicit(obj, arg, order) \
	__atomic_fetch_sub(&(obj)->__val, (arg), (order))
#define atomic_fetch_sub(obj, arg) \
	__atomic_fetch_sub(&(obj)->__val, (arg), __ATOMIC_SEQ_CST)

#define atomic_fetch_or_explicit(obj, arg, order) \
	__atomic_fetch_or(&(obj)->__val, (arg), (order))
#define atomic_fetch_or(obj, arg) \
	__atomic_fetch_or(&(obj)->__val, (arg), __ATOMIC_SEQ_CST)

#define atomic_fetch_xor_explicit(obj, arg, order) \
	__atomic_fetch_xor(&(obj)->__val, (arg), (order))
#define atomic_fetch_xor(obj, arg) \
	__atomic_fetch_xor(&(obj)->__val, (arg), __ATOMIC_SEQ_CST)

#define atomic_fetch_and_explicit(obj, arg, order) \
	__atomic_fetch_and(&(obj)->__val, (arg), (order))
#define atomic_fetch_and(obj, arg) \
	__atomic_fetch_and(&(obj)->__val, (arg), __ATOMIC_SEQ_CST)

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)

#define atomic_flag_test_and_set_explicit(flag, order) \
	__atomic_test_and_set(&(flag)->__val, (order))
#define atomic_flag_test_and_set(flag) \
	__atomic_test_and_set(&(flag)->__val, __ATOMIC_SEQ_CST)

#define atomic_flag_clear_explicit(flag, order) \
	__atomic_clear(&(flag)->__val, (order))
#define atomic_flag_clear(flag) __atomic_clear(&(flag)->__val, __ATOMIC_SEQ_CST)

#endif
