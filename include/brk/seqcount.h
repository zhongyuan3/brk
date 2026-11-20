#ifndef BRK_SEQCOUNT_H
#define BRK_SEQCOUNT_H

#include <brk/types.h>
#include <stdbool.h>

/*
 * Minimal sequence counter for lockless readers with a single writer.
 *
 * The writer must serialize itself; the sequence only tells readers when a
 * snapshot may have been torn.  This is the same model as Linux's seqcount_t.
 */
typedef struct seqcount {
	u32 sequence;
} seqcount_t;

#define SEQCOUNT_INITIALIZER { 0 }

static inline void seqcount_init(seqcount_t *s)
{
	__atomic_store_n(&s->sequence, 0, __ATOMIC_RELAXED);
}

static inline u32 read_seqcount_begin(const seqcount_t *s)
{
	u32 seq;

	do {
		seq = __atomic_load_n(&s->sequence, __ATOMIC_ACQUIRE);
	} while (seq & 1);

	return seq;
}

static inline bool read_seqcount_retry(const seqcount_t *s, u32 start)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&s->sequence, __ATOMIC_RELAXED) != start;
}

static inline void write_seqcount_begin(seqcount_t *s)
{
	u32 seq = __atomic_load_n(&s->sequence, __ATOMIC_RELAXED);

	__atomic_store_n(&s->sequence, seq + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void write_seqcount_end(seqcount_t *s)
{
	u32 seq = __atomic_load_n(&s->sequence, __ATOMIC_RELAXED);

	__atomic_store_n(&s->sequence, seq + 1, __ATOMIC_RELEASE);
}

#endif
