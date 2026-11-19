/**
 * @file mm/mchain.c
 * @brief Early physical memory allocator.
 *
 * The global pool is statically initialized and the free-entry list is built
 * lazily by mchain_bootstrap().  Regions are kept in an address-ordered,
 * doubly linked list whose nodes live in a fixed array.  ALLOCATED entries
 * never merge, which keeps mchain_free() exact.
 */

#include <arch/asm/page.h>
#include <arch/mm.h>
#include <brk/align.h>
#include <brk/assert.h>
#include <brk/log2.h>
#include <brk/mchain.h>
#include <brk/min_max.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <uapi/errno.h>

struct mchain_name {
	char name[MCHAIN_NAME_MAX];
	bool used;
};

static struct mchain_entry mchain_static_entries[MCHAIN_STATIC_CAPACITY];
static struct mchain_name mchain_names[MCHAIN_NAME_SLOTS];

struct mchain_pool mchain_pool_instance = {
	.entries = mchain_static_entries,
	.capacity = MCHAIN_STATIC_CAPACITY,
	.used = 0,
	.head = -1,
	.free_head = -1,
	.bottom_up = true,
	.can_grow = 0,
	.frozen = 0,
	.free_list_ready = 0,
	.min_limit = 0,
	.max_limit = MCHAIN_ANY_ADDR,
	.peak_used = 0,
	.grow_count = 0,
	.alloc_failures = 0,
};

static unsigned int mchain_state(const struct mchain_entry *entry)
{
	return entry->flags & MCHAIN_STATE_MASK;
}

static unsigned int mchain_attrs(const struct mchain_entry *entry)
{
	return entry->flags & MCHAIN_ATTR_MASK;
}

static bool mchain_state_valid(unsigned int state)
{
	return state == MCHAIN_STATE_FREE || state == MCHAIN_STATE_RESERVED ||
	       state == MCHAIN_STATE_ALLOCATED;
}

static bool mchain_attrs_valid(unsigned int attrs)
{
	return (attrs & ~MCHAIN_ATTR_MASK) == 0;
}

static bool mchain_nid_match(int entry_nid, int nid)
{
	return nid == MCHAIN_NID_ANY || entry_nid == nid ||
	       entry_nid == MCHAIN_NID_ANY;
}

static unsigned int mchain_attr_skip_mask(unsigned int attr_mask)
{
	unsigned int skip = 0;

	if (!(attr_mask & MCHAIN_ATTR_NOMAP))
		skip |= MCHAIN_ATTR_NOMAP;
	if (!(attr_mask & MCHAIN_ATTR_DRIVER))
		skip |= MCHAIN_ATTR_DRIVER;

	return skip;
}

static bool mchain_attrs_match(const struct mchain_entry *entry,
			       unsigned int attr_mask, unsigned int skip_mask)
{
	unsigned int attrs = mchain_attrs(entry);

	return (attrs & attr_mask) == attr_mask && (attrs & skip_mask) == 0;
}

static bool mchain_mergeable(const struct mchain_entry *a,
			     const struct mchain_entry *b)
{
	unsigned int state_a = mchain_state(a);
	unsigned int state_b = mchain_state(b);

	if (state_a == MCHAIN_STATE_ALLOCATED || state_a != state_b)
		return false;
	if (mchain_attrs(a) != mchain_attrs(b))
		return false;
	if (a->nid != b->nid || a->name_id != b->name_id)
		return false;

	return a->base + a->size == b->base;
}

static int mchain_name_lookup(const char *name, bool create, unsigned int *out)
{
	size_t len;
	unsigned int i;

	if (!name || !*name || !out)
		return -EINVAL;

	len = strlen(name);
	if (len >= MCHAIN_NAME_MAX)
		return -EINVAL;

	for (i = 1; i < MCHAIN_NAME_SLOTS; i++) {
		if (mchain_names[i].used &&
		    strcmp(mchain_names[i].name, name) == 0) {
			*out = i;
			return 0;
		}
	}

	if (!create)
		return -ENOENT;

	for (i = 1; i < MCHAIN_NAME_SLOTS; i++) {
		if (!mchain_names[i].used) {
			mchain_names[i].used = true;
			strlcpy(mchain_names[i].name, name, MCHAIN_NAME_MAX);
			*out = i;
			return 0;
		}
	}

	return -ENOSPC;
}

static void mchain_free_list_push(struct mchain_pool *pool, int idx)
{
	struct mchain_entry *entry;

	assert(pool);
	assert(idx >= 0);
	assert((unsigned int)idx < pool->capacity);
	assert(pool->free_list_ready);

	entry = &pool->entries[idx];
	entry->prev = -1;
	entry->next = pool->free_head;
	pool->free_head = idx;
}

static int mchain_free_list_pop(struct mchain_pool *pool)
{
	int idx;

	if (!pool || !pool->free_list_ready)
		return -1;

	idx = pool->free_head;
	if (idx < 0)
		return -1;

	assert((unsigned int)idx < pool->capacity);
	pool->free_head = pool->entries[idx].next;
	pool->entries[idx].next = -1;
	pool->entries[idx].prev = -1;

	return idx;
}

static void mchain_used_inc(struct mchain_pool *pool)
{
	assert(pool->used < pool->capacity);
	pool->used++;
	if (pool->used > pool->peak_used)
		pool->peak_used = pool->used;
}

static void mchain_used_dec(struct mchain_pool *pool)
{
	assert(pool->used > 0);
	if (pool->used > 0)
		pool->used--;
}

void mchain_bootstrap(struct mchain_pool *pool)
{
	unsigned int i;

	if (!pool || pool->free_list_ready)
		return;

	if (!pool->entries || pool->capacity == 0) {
		pool->free_head = -1;
		pool->free_list_ready = 1;
		return;
	}

	assert(pool->head == -1);
	assert(pool->free_head == -1);
	assert(pool->used == 0);

	for (i = 0; i < pool->capacity; i++) {
		pool->entries[i].prev = -1;
		pool->entries[i].next =
			(i + 1 < pool->capacity) ? (int)(i + 1) : -1;
	}

	pool->free_head = 0;
	pool->free_list_ready = 1;
}

void mchain_unlink_entry(struct mchain_pool *pool, int idx)
{
	struct mchain_entry *entry;

	if (!pool || idx < 0 || (unsigned int)idx >= pool->capacity)
		return;

	entry = &pool->entries[idx];

	if (entry->prev != -1)
		pool->entries[entry->prev].next = entry->next;
	else
		pool->head = entry->next;

	if (entry->next != -1)
		pool->entries[entry->next].prev = entry->prev;

	entry->prev = -1;
	entry->next = -1;
	mchain_free_list_push(pool, idx);
	mchain_used_dec(pool);
}

int mchain_merge_neighbors(struct mchain_pool *pool, int idx)
{
	struct mchain_entry *entry;

	if (!pool || idx < 0 || (unsigned int)idx >= pool->capacity)
		return idx;

	if (mchain_state(&pool->entries[idx]) == MCHAIN_STATE_ALLOCATED)
		return idx;

	for (;;) {
		int next;
		int prev;

		entry = &pool->entries[idx];
		next = entry->next;
		if (next != -1 &&
		    mchain_mergeable(entry, &pool->entries[next])) {
			entry->size += pool->entries[next].size;
			mchain_unlink_entry(pool, next);
			continue;
		}

		prev = entry->prev;
		if (prev != -1 &&
		    mchain_mergeable(&pool->entries[prev], entry)) {
			pool->entries[prev].size += entry->size;
			mchain_unlink_entry(pool, idx);
			idx = prev;
			continue;
		}

		break;
	}

	return idx;
}

static void mchain_merge_all(struct mchain_pool *pool)
{
	int idx;

	if (!pool)
		return;

	idx = pool->head;
	while (idx != -1) {
		idx = mchain_merge_neighbors(pool, idx);
		idx = pool->entries[idx].next;
	}
}

int mchain_insert_entry(struct mchain_pool *pool, phys_addr_t base,
			phys_size_t size, unsigned int state,
			unsigned int attrs, int nid, unsigned int name_id)
{
	struct mchain_entry *entry;
	phys_addr_t end;
	int idx;
	int prev;
	int cur;

	if (!pool || !pool->entries)
		return -EINVAL;
	if (!size || base + size < base)
		return -EINVAL;
	if (!mchain_state_valid(state) || !mchain_attrs_valid(attrs))
		return -EINVAL;
	if (nid < MCHAIN_NID_ANY)
		return -EINVAL;
	if (state == MCHAIN_STATE_RESERVED) {
		if (name_id >= MCHAIN_NAME_SLOTS)
			return -EINVAL;
		if (name_id != MCHAIN_NAME_NONE && !mchain_names[name_id].used)
			return -EINVAL;
	} else if (name_id != MCHAIN_NAME_NONE) {
		return -EINVAL;
	}
	if (!pool->free_list_ready)
		return -EINVAL;

	end = base + size;

	prev = -1;
	cur = pool->head;
	while (cur != -1 && pool->entries[cur].base < base) {
		prev = cur;
		cur = pool->entries[cur].next;
	}

	if (prev != -1 &&
	    pool->entries[prev].base + pool->entries[prev].size > base)
		return -EINVAL;
	if (cur != -1 && end > pool->entries[cur].base)
		return -EINVAL;

	idx = mchain_free_list_pop(pool);
	if (idx < 0)
		return -ENOMEM;

	entry = &pool->entries[idx];
	entry->base = base;
	entry->size = size;
	entry->prev = prev;
	entry->next = cur;
	entry->nid = nid;
	entry->flags = state | attrs;
	entry->name_id = name_id;

	if (prev != -1)
		pool->entries[prev].next = idx;
	else
		pool->head = idx;
	if (cur != -1)
		pool->entries[cur].prev = idx;

	mchain_used_inc(pool);
	mchain_merge_neighbors(pool, idx);

	return 0;
}

int mchain_carve_range(struct mchain_pool *pool, phys_addr_t start,
		       phys_addr_t end, unsigned int state,
		       unsigned int attr_mask, int nid, phys_size_t *carved)
{
	phys_size_t carved_size = 0;
	int idx;

	if (!pool || !pool->entries)
		return -EINVAL;
	if (!mchain_state_valid(state) || nid < MCHAIN_NID_ANY)
		return -EINVAL;

	attr_mask &= MCHAIN_ATTR_MASK;

	if (start >= end) {
		if (carved)
			*carved = 0;
		return 0;
	}

	idx = pool->head;
	while (idx != -1) {
		struct mchain_entry *entry = &pool->entries[idx];
		phys_addr_t entry_base = entry->base;
		phys_addr_t entry_end = entry_base + entry->size;
		phys_addr_t s;
		phys_addr_t t;
		int next = entry->next;

		if (entry_end <= start) {
			idx = next;
			continue;
		}
		if (entry_base >= end)
			break;

		if (mchain_state(entry) != state ||
		    (mchain_attrs(entry) & attr_mask) != attr_mask ||
		    !mchain_nid_match(entry->nid, nid)) {
			idx = next;
			continue;
		}

		s = max(entry_base, start);
		t = min(entry_end, end);
		if (s >= t) {
			idx = next;
			continue;
		}

		carved_size += t - s;

		if (start <= entry_base && entry_end <= end) {
			mchain_unlink_entry(pool, idx);
		} else if (start <= entry_base) {
			entry->base = end;
			entry->size = entry_end - end;
		} else if (entry_end <= end) {
			entry->size = start - entry_base;
		} else {
			int new_idx = mchain_free_list_pop(pool);
			struct mchain_entry *right;

			if (new_idx < 0)
				return -ENOMEM;

			right = &pool->entries[new_idx];
			*right = *entry;
			right->base = end;
			right->size = entry_end - end;
			right->prev = idx;
			right->next = next;
			if (next != -1)
				pool->entries[next].prev = new_idx;
			entry->next = new_idx;
			entry->size = start - entry_base;
			mchain_used_inc(pool);
		}

		idx = next;
	}

	if (carved)
		*carved = carved_size;

	return 0;
}

static bool mchain_find_gap(struct mchain_pool *pool, phys_addr_t base,
			    phys_addr_t end, phys_addr_t *gap_start,
			    phys_addr_t *gap_end)
{
	phys_addr_t cur = base;
	int idx;

	for (idx = pool->head; idx != -1; idx = pool->entries[idx].next) {
		struct mchain_entry *entry = &pool->entries[idx];
		phys_addr_t entry_base = entry->base;
		phys_addr_t entry_end = entry_base + entry->size;

		if (entry_end <= cur)
			continue;
		if (entry_base >= end)
			break;

		if (entry_base > cur) {
			*gap_start = cur;
			*gap_end = min(entry_base, end);
			return *gap_start < *gap_end;
		}

		cur = entry_end;
		if (cur >= end)
			break;
	}

	if (cur < end) {
		*gap_start = cur;
		*gap_end = end;
		return true;
	}

	return false;
}

static unsigned int mchain_count_gaps(struct mchain_pool *pool,
				      phys_addr_t base, phys_addr_t end)
{
	phys_addr_t cur = base;
	unsigned int count = 0;
	int idx;

	for (idx = pool->head; idx != -1; idx = pool->entries[idx].next) {
		struct mchain_entry *entry = &pool->entries[idx];
		phys_addr_t entry_base = entry->base;
		phys_addr_t entry_end = entry_base + entry->size;

		if (entry_end <= cur)
			continue;
		if (entry_base >= end)
			break;

		if (entry_base > cur) {
			count++;
			cur = entry_base;
		}

		if (entry_end > cur)
			cur = entry_end;
		if (cur >= end)
			break;
	}

	if (cur < end)
		count++;

	return count;
}

static bool mchain_find_state_intersection(struct mchain_pool *pool,
					   unsigned int state, phys_addr_t base,
					   phys_addr_t end, int *out_idx,
					   phys_addr_t *out_start,
					   phys_addr_t *out_end)
{
	int idx;

	for (idx = pool->head; idx != -1; idx = pool->entries[idx].next) {
		struct mchain_entry *entry = &pool->entries[idx];
		phys_addr_t entry_base = entry->base;
		phys_addr_t entry_end = entry_base + entry->size;
		phys_addr_t s;
		phys_addr_t t;

		if (mchain_state(entry) != state)
			continue;
		if (entry_end <= base)
			continue;
		if (entry_base >= end)
			break;

		s = max(entry_base, base);
		t = min(entry_end, end);
		if (s < t) {
			*out_idx = idx;
			*out_start = s;
			*out_end = t;
			return true;
		}
	}

	return false;
}

static unsigned int mchain_split_needed(phys_addr_t base, phys_addr_t end,
					phys_addr_t s, phys_addr_t t)
{
	if (s == base && t == end)
		return 0;
	if (s == base || t == end)
		return 1;
	return 2;
}

static unsigned int mchain_state_needed(struct mchain_pool *pool,
					unsigned int state, phys_addr_t base,
					phys_addr_t end)
{
	unsigned int needed = 0;
	int idx;

	for (idx = pool->head; idx != -1; idx = pool->entries[idx].next) {
		struct mchain_entry *entry = &pool->entries[idx];
		phys_addr_t entry_base = entry->base;
		phys_addr_t entry_end = entry_base + entry->size;
		phys_addr_t s;
		phys_addr_t t;

		if (mchain_state(entry) != state)
			continue;
		if (entry_end <= base)
			continue;
		if (entry_base >= end)
			break;

		s = max(entry_base, base);
		t = min(entry_end, end);
		if (s < t)
			needed += mchain_split_needed(entry_base, entry_end, s,
						      t);
	}

	return needed;
}

static int mchain_tail(struct mchain_pool *pool)
{
	int idx = pool->head;

	if (!pool || idx == -1)
		return -1;

	while (pool->entries[idx].next != -1)
		idx = pool->entries[idx].next;

	return idx;
}

static unsigned int mchain_candidate_entries(const struct mchain_entry *entry,
					     phys_addr_t candidate,
					     phys_size_t size)
{
	phys_addr_t entry_end = entry->base + entry->size;
	phys_addr_t candidate_end = candidate + size;

	if (candidate == entry->base && candidate_end == entry_end)
		return 0;
	if (candidate == entry->base || candidate_end == entry_end)
		return 1;
	return 2;
}

static bool mchain_try_candidate(struct mchain_pool *pool, int idx,
				 phys_addr_t candidate, phys_size_t size,
				 unsigned int budget, int *out_idx,
				 phys_addr_t *out_addr,
				 unsigned int *out_needed)
{
	struct mchain_entry *entry = &pool->entries[idx];
	phys_addr_t entry_end = entry->base + entry->size;
	unsigned int needed;

	if (candidate == 0 || candidate < entry->base || candidate > entry_end)
		return false;
	if (size > entry_end - candidate)
		return false;

	needed = mchain_candidate_entries(entry, candidate, size);
	if (needed > budget)
		return false;

	*out_idx = idx;
	*out_addr = candidate;
	*out_needed = needed;
	return true;
}

static bool mchain_find_candidate(struct mchain_pool *pool, phys_size_t size,
				  phys_size_t align, phys_addr_t lo,
				  phys_addr_t hi, int nid,
				  unsigned int attr_mask,
				  unsigned int skip_mask, unsigned int budget,
				  int *out_idx, phys_addr_t *out_addr,
				  unsigned int *out_needed)
{
	int idx;

	if (!pool || !pool->entries || !size || !align || !is_power_of_2(align))
		return false;
	if (nid < MCHAIN_NID_ANY || lo >= hi || size > hi - lo)
		return false;

	attr_mask &= MCHAIN_ATTR_MASK;

	idx = pool->bottom_up ? pool->head : mchain_tail(pool);

	while (idx != -1) {
		struct mchain_entry *entry = &pool->entries[idx];
		int next = pool->bottom_up ? entry->next : entry->prev;

		if (mchain_state(entry) == MCHAIN_STATE_FREE &&
		    mchain_nid_match(entry->nid, nid) &&
		    mchain_attrs_match(entry, attr_mask, skip_mask)) {
			phys_addr_t entry_base = entry->base;
			phys_addr_t entry_end = entry_base + entry->size;
			phys_addr_t s = max(entry_base, lo);
			phys_addr_t t = min(entry_end, hi);

			if (s < t) {
				phys_addr_t first = 0;
				phys_addr_t last = 0;
				bool have_first = false;

				if (pool->bottom_up) {
					if (s <=
					    MCHAIN_ANY_ADDR - (align - 1)) {
						first = align_up(s, align);
						if (first == 0)
							first = align;
						have_first = true;
						if (mchain_try_candidate(
							    pool, idx, first,
							    size, budget,
							    out_idx, out_addr,
							    out_needed))
							return true;
					}

					if (t >= size) {
						last = align_down(t - size,
								  align);
						if (last != 0 &&
						    (!have_first ||
						     last != first) &&
						    mchain_try_candidate(
							    pool, idx, last,
							    size, budget,
							    out_idx, out_addr,
							    out_needed))
							return true;
					}
				} else {
					if (t >= size) {
						first = align_down(t - size,
								   align);
						have_first = true;
						if (mchain_try_candidate(
							    pool, idx, first,
							    size, budget,
							    out_idx, out_addr,
							    out_needed))
							return true;
					}

					if (s <=
					    MCHAIN_ANY_ADDR - (align - 1)) {
						last = align_up(s, align);
						if (last == 0)
							last = align;
						if ((!have_first ||
						     last != first) &&
						    mchain_try_candidate(
							    pool, idx, last,
							    size, budget,
							    out_idx, out_addr,
							    out_needed))
							return true;
					}
				}
			}
		}

		idx = next;
	}

	return false;
}

static bool mchain_find_arena_range(struct mchain_pool *pool, phys_size_t size,
				    phys_size_t align, phys_addr_t avoid_start,
				    phys_addr_t avoid_end,
				    phys_addr_t *out_addr,
				    unsigned int *out_attrs, int *out_nid)
{
	unsigned int skip = MCHAIN_ATTR_NOMAP | MCHAIN_ATTR_DRIVER |
			    MCHAIN_ATTR_HOTPLUG;
	unsigned int needed;
	phys_addr_t addr;
	int idx;

	if (!pool)
		return false;

	if (avoid_start < avoid_end) {
		phys_addr_t lo = pool->min_limit;
		phys_addr_t hi = min(avoid_start, pool->max_limit);

		if (lo < hi &&
		    mchain_find_candidate(pool, size, align, lo, hi,
					  MCHAIN_NID_ANY, 0, skip, UINT_MAX,
					  &idx, &addr, &needed))
			goto found;

		lo = max(avoid_end, pool->min_limit);
		hi = pool->max_limit;
		if (lo < hi &&
		    mchain_find_candidate(pool, size, align, lo, hi,
					  MCHAIN_NID_ANY, 0, skip, UINT_MAX,
					  &idx, &addr, &needed))
			goto found;

		return false;
	}

	if (!mchain_find_candidate(pool, size, align, pool->min_limit,
				   pool->max_limit, MCHAIN_NID_ANY, 0, skip,
				   UINT_MAX, &idx, &addr, &needed))
		return false;

found:
	*out_addr = addr;
	*out_attrs = mchain_attrs(&pool->entries[idx]);
	*out_nid = pool->entries[idx].nid;
	return true;
}

static int mchain_alloc_from_free_entry(struct mchain_pool *pool, int idx,
					phys_addr_t candidate, phys_size_t size,
					unsigned int attrs, int nid)
{
	struct mchain_entry *entry = &pool->entries[idx];
	phys_addr_t base = entry->base;
	phys_addr_t end = base + entry->size;
	int ret;

	if (candidate == base && candidate + size == end) {
		entry->flags = MCHAIN_STATE_ALLOCATED | attrs;
		entry->name_id = MCHAIN_NAME_NONE;
		return 0;
	}

	if (candidate == base) {
		entry->size = size;
		entry->flags = MCHAIN_STATE_ALLOCATED | attrs;
		entry->name_id = MCHAIN_NAME_NONE;
		return mchain_insert_entry(pool, candidate + size,
					   end - (candidate + size),
					   MCHAIN_STATE_FREE, attrs, nid,
					   MCHAIN_NAME_NONE);
	}

	if (candidate + size == end) {
		entry->base = candidate;
		entry->size = size;
		entry->flags = MCHAIN_STATE_ALLOCATED | attrs;
		entry->name_id = MCHAIN_NAME_NONE;
		return mchain_insert_entry(pool, base, candidate - base,
					   MCHAIN_STATE_FREE, attrs, nid,
					   MCHAIN_NAME_NONE);
	}

	entry->size = candidate - base;
	ret = mchain_insert_entry(pool, candidate, size, MCHAIN_STATE_ALLOCATED,
				  attrs, nid, MCHAIN_NAME_NONE);
	if (ret)
		return ret;

	return mchain_insert_entry(pool, candidate + size,
				   end - (candidate + size), MCHAIN_STATE_FREE,
				   attrs, nid, MCHAIN_NAME_NONE);
}

static int mchain_free_allocated(struct mchain_pool *pool, phys_addr_t base,
				 phys_size_t size)
{
	int idx;

	for (idx = pool->head; idx != -1; idx = pool->entries[idx].next) {
		struct mchain_entry *entry = &pool->entries[idx];

		if (mchain_state(entry) != MCHAIN_STATE_ALLOCATED)
			continue;
		if (entry->base != base || entry->size != size)
			continue;

		entry->flags = MCHAIN_STATE_FREE | mchain_attrs(entry);
		entry->name_id = MCHAIN_NAME_NONE;
		mchain_merge_all(pool);
		return 0;
	}

	return -ENOENT;
}

int mchain_grow_pool(struct mchain_pool *pool, phys_addr_t avoid_start,
		     phys_addr_t avoid_end)
{
	struct mchain_entry *old_entries;
	struct mchain_entry *new_entries;
	phys_addr_t new_pa;
	phys_size_t new_bytes;
	phys_size_t old_bytes;
	phys_size_t carved;
	unsigned int old_capacity;
	unsigned int new_capacity;
	unsigned int old_used;
	unsigned int src_attrs;
	unsigned int i;
	int src_nid;
	int src;
	int ret = -ENOMEM;
	bool old_static;

	if (!pool || !pool->entries)
		return -EINVAL;
	if (!pool->can_grow || pool->frozen)
		return -ENOMEM;

	old_entries = pool->entries;
	old_capacity = pool->capacity;
	old_used = pool->used;
	old_static = old_entries == mchain_static_entries;

	if (old_used > old_capacity)
		return -EINVAL;
	if (old_capacity >
	    ((unsigned int)INT_MAX - MCHAIN_GROW_HEADROOM) / MCHAIN_GROW_FACTOR)
		return -ENOMEM;

	new_capacity = old_capacity * MCHAIN_GROW_FACTOR + MCHAIN_GROW_HEADROOM;
	if (new_capacity <= old_capacity || old_used > new_capacity)
		return -ENOMEM;

	new_bytes = (phys_size_t)new_capacity * sizeof(struct mchain_entry);
	if (new_bytes > MCHAIN_ANY_ADDR - (MCHAIN_ARENA_ALIGN - 1))
		return -ENOMEM;
	new_bytes = align_up(new_bytes, MCHAIN_ARENA_ALIGN);
	if (!new_bytes)
		return -ENOMEM;

	pool->can_grow = 0;

	if (!mchain_find_arena_range(pool, new_bytes, MCHAIN_ARENA_ALIGN,
				     avoid_start, avoid_end, &new_pa,
				     &src_attrs, &src_nid))
		goto out;

	new_entries = (struct mchain_entry *)(uintptr_t)phys_to_virt(new_pa);
	if (!new_entries)
		goto out;

	src = pool->head;
	for (i = 0; i < old_used; i++) {
		if (src < 0 || (unsigned int)src >= old_capacity) {
			ret = -EINVAL;
			goto out;
		}

		new_entries[i] = old_entries[src];
		new_entries[i].prev = i ? (int)i - 1 : -1;
		new_entries[i].next = (i + 1 < old_used) ? (int)i + 1 : -1;
		src = old_entries[src].next;
	}
	if (src != -1) {
		ret = -EINVAL;
		goto out;
	}

	for (i = old_used; i < new_capacity; i++) {
		new_entries[i].prev = -1;
		new_entries[i].next = (i + 1 < new_capacity) ? (int)i + 1 : -1;
	}

	pool->entries = new_entries;
	pool->capacity = new_capacity;
	pool->used = old_used;
	pool->head = old_used ? 0 : -1;
	pool->free_head = (old_used < new_capacity) ? (int)old_used : -1;
	pool->free_list_ready = 1;

	ret = mchain_carve_range(pool, new_pa, new_pa + new_bytes,
				 MCHAIN_STATE_FREE, 0, MCHAIN_NID_ANY, &carved);
	if (ret || carved != new_bytes) {
		ret = -ENOMEM;
		goto out;
	}

	ret = mchain_insert_entry(pool, new_pa, new_bytes,
				  MCHAIN_STATE_ALLOCATED, src_attrs, src_nid,
				  MCHAIN_NAME_NONE);
	if (ret)
		goto out;

	if (!old_static) {
		old_bytes =
			(phys_size_t)old_capacity * sizeof(struct mchain_entry);
		old_bytes = align_up(old_bytes, MCHAIN_ARENA_ALIGN);
		ret = mchain_free_allocated(
			pool, virt_to_phys((virt_addr_t)(uintptr_t)old_entries),
			old_bytes);
		if (ret)
			goto out;
	}

	pool->grow_count++;
	ret = 0;

out:
	pool->can_grow = 1;
	return ret;
}

int mchain_ensure_entries(struct mchain_pool *pool, unsigned int nr,
			  phys_addr_t avoid_start, phys_addr_t avoid_end)
{
	unsigned int free_entries;
	int ret;

	if (!pool)
		return -EINVAL;
	if (pool->used > pool->capacity)
		return -EINVAL;
	if (!pool->free_list_ready)
		mchain_bootstrap(pool);
	if (pool->frozen)
		return -EBUSY;

	while ((free_entries = pool->capacity - pool->used) < nr) {
		if (!pool->can_grow)
			return -ENOMEM;

		ret = mchain_grow_pool(pool, avoid_start, avoid_end);
		if (ret) {
			if (pool->capacity - pool->used >= nr)
				return 0;
			return ret;
		}
	}

	return 0;
}

static int mchain_reserve_free_part(struct mchain_pool *pool, int idx,
				    phys_addr_t start, phys_addr_t end,
				    unsigned int attrs, int nid,
				    unsigned int name_id)
{
	struct mchain_entry *entry = &pool->entries[idx];
	phys_addr_t base = entry->base;
	phys_addr_t entry_end = base + entry->size;
	unsigned int src_attrs = mchain_attrs(entry);
	unsigned int new_flags = MCHAIN_STATE_RESERVED | (src_attrs | attrs);
	int src_nid = entry->nid;
	int ret;

	if (start == base && end == entry_end) {
		entry->flags = new_flags;
		entry->nid = nid;
		entry->name_id = name_id;
		return 0;
	}

	if (start == base) {
		entry->size = end - base;
		entry->flags = new_flags;
		entry->nid = nid;
		entry->name_id = name_id;
		return mchain_insert_entry(pool, end, entry_end - end,
					   MCHAIN_STATE_FREE, src_attrs,
					   src_nid, MCHAIN_NAME_NONE);
	}

	if (end == entry_end) {
		entry->size = start - base;
		return mchain_insert_entry(pool, start, end - start,
					   MCHAIN_STATE_RESERVED,
					   src_attrs | attrs, nid, name_id);
	}

	entry->size = start - base;
	ret = mchain_insert_entry(pool, start, end - start,
				  MCHAIN_STATE_RESERVED, src_attrs | attrs, nid,
				  name_id);
	if (ret)
		return ret;

	return mchain_insert_entry(pool, end, entry_end - end,
				   MCHAIN_STATE_FREE, src_attrs, src_nid,
				   MCHAIN_NAME_NONE);
}

static int mchain_unreserve_reserved_part(struct mchain_pool *pool, int idx,
					  phys_addr_t start, phys_addr_t end)
{
	struct mchain_entry *entry = &pool->entries[idx];
	phys_addr_t base = entry->base;
	phys_addr_t entry_end = base + entry->size;
	unsigned int src_attrs = mchain_attrs(entry);
	unsigned int src_name = entry->name_id;
	int src_nid = entry->nid;
	int ret;

	if (start == base && end == entry_end) {
		entry->flags = MCHAIN_STATE_FREE | src_attrs;
		entry->name_id = MCHAIN_NAME_NONE;
		return 0;
	}

	if (start == base) {
		entry->size = end - base;
		entry->flags = MCHAIN_STATE_FREE | src_attrs;
		entry->name_id = MCHAIN_NAME_NONE;
		return mchain_insert_entry(pool, end, entry_end - end,
					   MCHAIN_STATE_RESERVED, src_attrs,
					   src_nid, src_name);
	}

	if (end == entry_end) {
		entry->size = start - base;
		return mchain_insert_entry(pool, start, end - start,
					   MCHAIN_STATE_FREE, src_attrs,
					   src_nid, MCHAIN_NAME_NONE);
	}

	entry->size = start - base;
	ret = mchain_insert_entry(pool, start, end - start, MCHAIN_STATE_FREE,
				  src_attrs, src_nid, MCHAIN_NAME_NONE);
	if (ret)
		return ret;

	return mchain_insert_entry(pool, end, entry_end - end,
				   MCHAIN_STATE_RESERVED, src_attrs, src_nid,
				   src_name);
}

int mchain_set_limit(phys_addr_t min_addr, phys_addr_t max_addr)
{
	phys_addr_t min = min_addr == MCHAIN_ANY_ADDR ? 0 : min_addr;

	if (min >= max_addr)
		return -EINVAL;

	mchain_pool_instance.min_limit = min;
	mchain_pool_instance.max_limit = max_addr;
	return 0;
}

void mchain_set_bottom_up(bool bottom_up)
{
	mchain_pool_instance.bottom_up = bottom_up;
}

bool mchain_bottom_up(void)
{
	return mchain_pool_instance.bottom_up;
}

void mchain_allow_grow(void)
{
	if (!mchain_pool_instance.frozen)
		mchain_pool_instance.can_grow = 1;
}

void mchain_freeze(void)
{
	mchain_pool_instance.frozen = 1;
	mchain_pool_instance.can_grow = 0;
}

static int mchain_region_args_valid(phys_addr_t base, phys_size_t size,
				    unsigned int attrs, int nid)
{
	if (!size || base + size < base)
		return -EINVAL;
	if (!mchain_attrs_valid(attrs) || nid < MCHAIN_NID_ANY)
		return -EINVAL;
	return 0;
}

int mchain_add(phys_addr_t base, phys_size_t size, unsigned int attrs, int nid)
{
	phys_addr_t end;
	unsigned int gaps;
	int ret;

	if (mchain_pool_instance.frozen)
		return -EBUSY;
	if (!size)
		return 0;

	ret = mchain_region_args_valid(base, size, attrs, nid);
	if (ret)
		return ret;

	mchain_bootstrap(&mchain_pool_instance);

	end = base + size;
	gaps = mchain_count_gaps(&mchain_pool_instance, base, end);
	if (!gaps)
		return 0;

	ret = mchain_ensure_entries(&mchain_pool_instance, gaps, base, end);
	if (ret)
		return ret;

	for (;;) {
		phys_addr_t gap_start;
		phys_addr_t gap_end;

		if (!mchain_find_gap(&mchain_pool_instance, base, end,
				     &gap_start, &gap_end))
			break;

		ret = mchain_insert_entry(&mchain_pool_instance, gap_start,
					  gap_end - gap_start,
					  MCHAIN_STATE_FREE, attrs, nid,
					  MCHAIN_NAME_NONE);
		if (ret) {
			assert(0);
			return ret;
		}
	}

	mchain_merge_all(&mchain_pool_instance);
	return 0;
}

static int mchain_reserve_common(phys_addr_t base, phys_size_t size,
				 unsigned int attrs, int nid,
				 unsigned int name_id)
{
	phys_addr_t end;
	unsigned int needed;
	int ret;

	if (mchain_pool_instance.frozen)
		return -EBUSY;
	if (!size)
		return 0;

	ret = mchain_region_args_valid(base, size, attrs, nid);
	if (ret)
		return ret;
	if (name_id >= MCHAIN_NAME_SLOTS)
		return -EINVAL;
	if (name_id != MCHAIN_NAME_NONE && !mchain_names[name_id].used)
		return -EINVAL;

	mchain_bootstrap(&mchain_pool_instance);

	end = base + size;
	needed = mchain_state_needed(&mchain_pool_instance, MCHAIN_STATE_FREE,
				     base, end);
	if (needed) {
		ret = mchain_ensure_entries(&mchain_pool_instance, needed, base,
					    end);
		if (ret)
			return ret;
	}

	for (;;) {
		phys_addr_t start;
		phys_addr_t stop;
		int idx;

		if (!mchain_find_state_intersection(&mchain_pool_instance,
						    MCHAIN_STATE_FREE, base,
						    end, &idx, &start, &stop))
			break;

		ret = mchain_reserve_free_part(&mchain_pool_instance, idx,
					       start, stop, attrs, nid,
					       name_id);
		if (ret) {
			assert(0);
			return ret;
		}
	}

	mchain_merge_all(&mchain_pool_instance);
	return 0;
}

int mchain_reserve(phys_addr_t base, phys_size_t size, unsigned int attrs,
		   int nid)
{
	return mchain_reserve_common(base, size, attrs, nid, MCHAIN_NAME_NONE);
}

int mchain_reserve_named(const char *name, phys_addr_t base, phys_size_t size,
			 unsigned int attrs, int nid)
{
	unsigned int name_id;
	int ret;

	if (mchain_pool_instance.frozen)
		return -EBUSY;
	if (!size)
		return 0;
	if (!name || !*name)
		return -EINVAL;

	ret = mchain_region_args_valid(base, size, attrs, nid);
	if (ret)
		return ret;

	ret = mchain_name_lookup(name, true, &name_id);
	if (ret)
		return ret;

	return mchain_reserve_common(base, size, attrs, nid, name_id);
}

int mchain_find_reserved(const char *name, phys_addr_t *base, phys_size_t *size)
{
	unsigned int name_id;
	int idx;
	int ret;

	if (!name || !base || !size)
		return -EINVAL;

	ret = mchain_name_lookup(name, false, &name_id);
	if (ret)
		return ret;

	for (idx = mchain_pool_instance.head; idx != -1;
	     idx = mchain_pool_instance.entries[idx].next) {
		struct mchain_entry *entry = &mchain_pool_instance.entries[idx];

		if (mchain_state(entry) != MCHAIN_STATE_RESERVED)
			continue;
		if (entry->name_id != name_id)
			continue;

		*base = entry->base;
		*size = entry->size;
		return 0;
	}

	return -ENOENT;
}

int mchain_unreserve(phys_addr_t base, phys_size_t size)
{
	phys_addr_t end;
	unsigned int needed;
	int ret;

	if (mchain_pool_instance.frozen)
		return -EBUSY;
	if (!size)
		return 0;
	if (base + size < base)
		return -EINVAL;

	mchain_bootstrap(&mchain_pool_instance);

	end = base + size;
	needed = mchain_state_needed(&mchain_pool_instance,
				     MCHAIN_STATE_RESERVED, base, end);
	if (needed) {
		ret = mchain_ensure_entries(&mchain_pool_instance, needed, base,
					    end);
		if (ret)
			return ret;
	}

	for (;;) {
		phys_addr_t start;
		phys_addr_t stop;
		int idx;

		if (!mchain_find_state_intersection(&mchain_pool_instance,
						    MCHAIN_STATE_RESERVED, base,
						    end, &idx, &start, &stop))
			break;

		ret = mchain_unreserve_reserved_part(&mchain_pool_instance, idx,
						     start, stop);
		if (ret) {
			assert(0);
			return ret;
		}
	}

	mchain_merge_all(&mchain_pool_instance);
	return 0;
}

static bool mchain_effective_limits(phys_addr_t min_addr, phys_addr_t max_addr,
				    phys_addr_t *lo, phys_addr_t *hi)
{
	phys_addr_t lower = min_addr == MCHAIN_ANY_ADDR ? 0 : min_addr;
	phys_addr_t upper = max_addr == MCHAIN_ANY_ADDR ? MCHAIN_ANY_ADDR :
							  max_addr;

	lower = max(lower, mchain_pool_instance.min_limit);
	upper = min(upper, mchain_pool_instance.max_limit);
	if (lower >= upper)
		return false;

	*lo = lower;
	*hi = upper;
	return true;
}

phys_addr_t mchain_find_in_range(phys_size_t size, phys_size_t align,
				 phys_addr_t min_addr, phys_addr_t max_addr,
				 int nid, unsigned int attr_mask)
{
	phys_addr_t lo;
	phys_addr_t hi;
	phys_addr_t addr;
	unsigned int needed;
	unsigned int skip;
	int idx;

	if (mchain_pool_instance.frozen)
		return 0;
	if (!size || !align || !is_power_of_2(align) || nid < MCHAIN_NID_ANY)
		return 0;
	if (!mchain_effective_limits(min_addr, max_addr, &lo, &hi))
		return 0;
	if (size > hi - lo)
		return 0;

	attr_mask &= MCHAIN_ATTR_MASK;
	skip = mchain_attr_skip_mask(attr_mask);

	if (!mchain_find_candidate(&mchain_pool_instance, size, align, lo, hi,
				   nid, attr_mask, skip, UINT_MAX, &idx, &addr,
				   &needed))
		return 0;

	return addr;
}

phys_addr_t mchain_alloc(phys_size_t size, phys_size_t align,
			 phys_addr_t min_addr, phys_addr_t max_addr, int nid,
			 unsigned int attr_mask)
{
	phys_addr_t lo;
	phys_addr_t hi;
	phys_addr_t addr = 0;
	unsigned int needed;
	unsigned int skip;
	unsigned int budget;
	unsigned int old_capacity;
	int idx;
	int ret;

	if (mchain_pool_instance.frozen)
		goto fail;
	if (!size || !align || !is_power_of_2(align) || nid < MCHAIN_NID_ANY)
		goto fail;
	if (!mchain_effective_limits(min_addr, max_addr, &lo, &hi))
		goto fail;
	if (size > hi - lo)
		goto fail;

	attr_mask &= MCHAIN_ATTR_MASK;
	skip = mchain_attr_skip_mask(attr_mask);

	mchain_bootstrap(&mchain_pool_instance);
	if (mchain_pool_instance.used > mchain_pool_instance.capacity)
		goto fail;

	for (;;) {
		budget = mchain_pool_instance.capacity -
			 mchain_pool_instance.used;

		if (mchain_find_candidate(&mchain_pool_instance, size, align,
					  lo, hi, nid, attr_mask, skip, budget,
					  &idx, &addr, &needed))
			break;

		if (!mchain_pool_instance.can_grow)
			goto fail;

		if (!mchain_find_candidate(&mchain_pool_instance, size, align,
					   lo, hi, nid, attr_mask, skip,
					   UINT_MAX, &idx, &addr, &needed))
			goto fail;

		old_capacity = mchain_pool_instance.capacity;
		ret = mchain_ensure_entries(&mchain_pool_instance, needed, addr,
					    addr + size);
		if (mchain_pool_instance.capacity == old_capacity)
			goto fail;
	}

	ret = mchain_alloc_from_free_entry(
		&mchain_pool_instance, idx, addr, size,
		mchain_attrs(&mchain_pool_instance.entries[idx]),
		mchain_pool_instance.entries[idx].nid);
	if (ret)
		goto fail;

	mchain_merge_all(&mchain_pool_instance);
	return addr;

fail:
	mchain_pool_instance.alloc_failures++;
	return 0;
}

int mchain_free(phys_addr_t base, phys_size_t size)
{
	if (mchain_pool_instance.frozen)
		return -EBUSY;
	if (!size || base + size < base)
		return -EINVAL;

	mchain_bootstrap(&mchain_pool_instance);
	return mchain_free_allocated(&mchain_pool_instance, base, size);
}

bool mchain_range_is_free(phys_addr_t base, phys_size_t size)
{
	phys_addr_t end;
	phys_addr_t cur;

	if (!size)
		return true;
	if (base + size < base)
		return false;

	end = base + size;
	cur = base;

	for (int idx = mchain_pool_instance.head; idx != -1;
	     idx = mchain_pool_instance.entries[idx].next) {
		struct mchain_entry *entry = &mchain_pool_instance.entries[idx];
		phys_addr_t entry_base = entry->base;
		phys_addr_t entry_end = entry_base + entry->size;

		if (entry_end <= cur)
			continue;
		if (entry_base > cur)
			return false;
		if (mchain_state(entry) != MCHAIN_STATE_FREE)
			return false;

		cur = entry_end;
		if (cur >= end)
			return true;
	}

	return false;
}

bool mchain_is_memory(phys_addr_t addr)
{
	int idx;

	for (idx = mchain_pool_instance.head; idx != -1;
	     idx = mchain_pool_instance.entries[idx].next) {
		struct mchain_entry *entry = &mchain_pool_instance.entries[idx];

		if (addr >= entry->base && addr - entry->base < entry->size)
			return true;
	}

	return false;
}

phys_size_t mchain_free_size(void)
{
	phys_size_t total = 0;
	int idx;

	for (idx = mchain_pool_instance.head; idx != -1;
	     idx = mchain_pool_instance.entries[idx].next) {
		struct mchain_entry *entry = &mchain_pool_instance.entries[idx];

		if (mchain_state(entry) == MCHAIN_STATE_FREE)
			total += entry->size;
	}

	return total;
}

phys_size_t mchain_reserved_size(void)
{
	phys_size_t total = 0;
	int idx;

	for (idx = mchain_pool_instance.head; idx != -1;
	     idx = mchain_pool_instance.entries[idx].next) {
		struct mchain_entry *entry = &mchain_pool_instance.entries[idx];
		unsigned int state = mchain_state(entry);

		if (state == MCHAIN_STATE_RESERVED ||
		    state == MCHAIN_STATE_ALLOCATED)
			total += entry->size;
	}

	return total;
}

phys_size_t mchain_memory_size(void)
{
	phys_size_t total = 0;
	int idx;

	for (idx = mchain_pool_instance.head; idx != -1;
	     idx = mchain_pool_instance.entries[idx].next)
		total += mchain_pool_instance.entries[idx].size;

	return total;
}

phys_addr_t mchain_start_of_dram(void)
{
	if (mchain_pool_instance.head == -1)
		return 0;

	return mchain_pool_instance.entries[mchain_pool_instance.head].base;
}

phys_addr_t mchain_end_of_dram(void)
{
	int idx = mchain_tail(&mchain_pool_instance);

	if (idx == -1)
		return 0;

	return mchain_pool_instance.entries[idx].base +
	       mchain_pool_instance.entries[idx].size;
}

unsigned int mchain_used_entries(void)
{
	return mchain_pool_instance.used;
}

unsigned int mchain_capacity(void)
{
	return mchain_pool_instance.capacity;
}

void mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr,
		      phys_addr_t max_addr, int nid, unsigned int attr_mask)
{
	if (!it)
		return;

	it->min_addr = min_addr == MCHAIN_ANY_ADDR ? 0 : min_addr;
	it->max_addr = max_addr == MCHAIN_ANY_ADDR ? MCHAIN_ANY_ADDR : max_addr;
	it->nid = nid;
	it->attr_mask = attr_mask & MCHAIN_ATTR_MASK;
	it->cur = it->min_addr < it->max_addr ? mchain_pool_instance.head : -1;
}

int mchain_iter_next(struct mchain_iter *it, phys_addr_t *start,
		     phys_addr_t *end)
{
	unsigned int skip;

	if (!it || !start || !end)
		return 0;

	skip = mchain_attr_skip_mask(it->attr_mask);

	while (it->cur != -1) {
		struct mchain_entry *entry =
			&mchain_pool_instance.entries[it->cur];
		phys_addr_t entry_base = entry->base;
		phys_addr_t entry_end = entry_base + entry->size;
		phys_addr_t s;
		phys_addr_t t;

		it->cur = entry->next;

		if (mchain_state(entry) != MCHAIN_STATE_FREE)
			continue;
		if (!mchain_nid_match(entry->nid, it->nid))
			continue;
		if (!mchain_attrs_match(entry, it->attr_mask, skip))
			continue;

		s = max(entry_base, it->min_addr);
		t = min(entry_end, it->max_addr);
		if (s >= t)
			continue;

		*start = s;
		*end = t;
		return 1;
	}

	return 0;
}

void mchain_get_stats(struct mchain_stats *stats)
{
	phys_size_t free_bytes = 0;
	phys_size_t reserved_bytes = 0;
	phys_size_t allocated_bytes = 0;
	int idx;

	if (!stats)
		return;

	for (idx = mchain_pool_instance.head; idx != -1;
	     idx = mchain_pool_instance.entries[idx].next) {
		struct mchain_entry *entry = &mchain_pool_instance.entries[idx];

		switch (mchain_state(entry)) {
		case MCHAIN_STATE_FREE:
			free_bytes += entry->size;
			break;
		case MCHAIN_STATE_RESERVED:
			reserved_bytes += entry->size;
			break;
		case MCHAIN_STATE_ALLOCATED:
			allocated_bytes += entry->size;
			break;
		default:
			break;
		}
	}

	stats->used_entries = mchain_pool_instance.used;
	stats->peak_used_entries = mchain_pool_instance.peak_used;
	stats->capacity = mchain_pool_instance.capacity;
	stats->grow_count = mchain_pool_instance.grow_count;
	stats->alloc_failures = mchain_pool_instance.alloc_failures;
	stats->frozen = mchain_pool_instance.frozen;
	stats->free_bytes = free_bytes;
	stats->reserved_bytes = reserved_bytes;
	stats->allocated_bytes = allocated_bytes;
}

static const char *mchain_state_name(unsigned int state)
{
	switch (state) {
	case MCHAIN_STATE_FREE:
		return "FREE";
	case MCHAIN_STATE_RESERVED:
		return "RESERVED";
	case MCHAIN_STATE_ALLOCATED:
		return "ALLOCATED";
	default:
		return "?";
	}
}

static void mchain_format_attrs(unsigned int attrs, char *buf, size_t size)
{
	if (!size)
		return;

	buf[0] = '\0';
	if (attrs & MCHAIN_ATTR_HOTPLUG)
		strlcat(buf, "HOTPLUG ", size);
	if (attrs & MCHAIN_ATTR_NOMAP)
		strlcat(buf, "NOMAP ", size);
	if (attrs & MCHAIN_ATTR_MIRROR)
		strlcat(buf, "MIRROR ", size);
	if (attrs & MCHAIN_ATTR_DRIVER)
		strlcat(buf, "DRIVER ", size);
}

void mchain_dump(void)
{
	int idx;

	mchain_bootstrap(&mchain_pool_instance);

	pr_info("mchain: arena=%p capacity=%u used=%u head=%d free_head=%d\n",
		(void *)mchain_pool_instance.entries,
		mchain_pool_instance.capacity, mchain_pool_instance.used,
		mchain_pool_instance.head, mchain_pool_instance.free_head);

	for (idx = mchain_pool_instance.head; idx != -1;
	     idx = mchain_pool_instance.entries[idx].next) {
		struct mchain_entry *entry = &mchain_pool_instance.entries[idx];
		char attrs[64];
		const char *name = "-";

		mchain_format_attrs(mchain_attrs(entry), attrs, sizeof(attrs));
		if (entry->name_id != MCHAIN_NAME_NONE &&
		    entry->name_id < MCHAIN_NAME_SLOTS &&
		    mchain_names[entry->name_id].used)
			name = mchain_names[entry->name_id].name;

		pr_info("mchain: [%d] [%llx, %llx) %s attrs=%s nid=%d name=%s prev=%d next=%d\n",
			idx, (unsigned long long)entry->base,
			(unsigned long long)(entry->base + entry->size),
			mchain_state_name(mchain_state(entry)), attrs,
			entry->nid, name, entry->prev, entry->next);
	}
}

static bool mchain_chain_contains(struct mchain_pool *pool, int idx)
{
	int cur;

	for (cur = pool->head; cur != -1; cur = pool->entries[cur].next) {
		if (cur == idx)
			return true;
	}

	return false;
}

static bool mchain_name_id_valid(unsigned int name_id)
{
	if (name_id == MCHAIN_NAME_NONE)
		return true;
	if (name_id >= MCHAIN_NAME_SLOTS)
		return false;
	return mchain_names[name_id].used;
}

int mchain_validate(void)
{
	struct mchain_pool *pool = &mchain_pool_instance;
	phys_size_t free_bytes = 0;
	phys_size_t reserved_bytes = 0;
	phys_size_t allocated_bytes = 0;
	unsigned int chain_count = 0;
	unsigned int free_count = 0;
	int idx;
	int prev;

	if (!pool->entries || pool->capacity == 0)
		return -EINVAL;
	if (pool->used > pool->capacity)
		return -EINVAL;

	if (!pool->free_list_ready) {
		if (pool->head != -1 || pool->free_head != -1 ||
		    pool->used != 0)
			return -EINVAL;
		mchain_bootstrap(pool);
	}

	if (!pool->free_list_ready)
		return -EINVAL;

	prev = -1;
	idx = pool->head;
	while (idx != -1) {
		struct mchain_entry *entry;
		unsigned int state;

		if (idx < 0 || (unsigned int)idx >= pool->capacity)
			return -EINVAL;
		if (chain_count >= pool->capacity)
			return -EINVAL;

		entry = &pool->entries[idx];
		if (entry->prev != prev)
			return -EINVAL;
		if (!entry->size || entry->base + entry->size < entry->base)
			return -EINVAL;

		state = mchain_state(entry);
		if (!mchain_state_valid(state))
			return -EINVAL;
		if (entry->flags & ~(MCHAIN_STATE_MASK | MCHAIN_ATTR_MASK))
			return -EINVAL;
		if (entry->nid < MCHAIN_NID_ANY)
			return -EINVAL;
		if (state != MCHAIN_STATE_RESERVED &&
		    entry->name_id != MCHAIN_NAME_NONE)
			return -EINVAL;
		if (state == MCHAIN_STATE_RESERVED &&
		    !mchain_name_id_valid(entry->name_id))
			return -EINVAL;

		if (prev != -1) {
			struct mchain_entry *prev_entry = &pool->entries[prev];

			if (prev_entry->base + prev_entry->size > entry->base)
				return -EINVAL;
			if (mchain_mergeable(prev_entry, entry))
				return -EINVAL;
		}

		switch (state) {
		case MCHAIN_STATE_FREE:
			free_bytes += entry->size;
			break;
		case MCHAIN_STATE_RESERVED:
			reserved_bytes += entry->size;
			break;
		case MCHAIN_STATE_ALLOCATED:
			allocated_bytes += entry->size;
			break;
		default:
			return -EINVAL;
		}

		prev = idx;
		idx = entry->next;
		chain_count++;
	}

	if (prev != -1 && pool->entries[prev].next != -1)
		return -EINVAL;
	if (chain_count != pool->used)
		return -EINVAL;
	if ((chain_count == 0) != (pool->head == -1))
		return -EINVAL;

	if (pool->capacity - pool->used == 0 && pool->free_head != -1)
		return -EINVAL;
	if (pool->capacity - pool->used != 0 && pool->free_head == -1)
		return -EINVAL;

	idx = pool->free_head;
	while (idx != -1) {
		int check;

		if (idx < 0 || (unsigned int)idx >= pool->capacity)
			return -EINVAL;
		if (free_count >= pool->capacity)
			return -EINVAL;
		if (mchain_chain_contains(pool, idx))
			return -EINVAL;

		check = pool->free_head;
		for (unsigned int i = 0; i < free_count; i++) {
			if (check == idx)
				return -EINVAL;
			if (check == -1)
				return -EINVAL;
			check = pool->entries[check].next;
		}

		idx = pool->entries[idx].next;
		free_count++;
	}

	if (free_count != pool->capacity - pool->used)
		return -EINVAL;
	if (pool->peak_used < pool->used)
		return -EINVAL;
	if ((pool->can_grow != 0 && pool->can_grow != 1) ||
	    (pool->frozen != 0 && pool->frozen != 1) ||
	    (pool->free_list_ready != 0 && pool->free_list_ready != 1))
		return -EINVAL;
	if (pool->frozen && pool->can_grow)
		return -EINVAL;
	if (free_bytes + reserved_bytes + allocated_bytes !=
	    mchain_memory_size())
		return -EINVAL;

	return 0;
}
