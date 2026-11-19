/**
 * @file brk/mchain.h
 * @brief Early physical memory allocator.
 *
 * mchain is an array-backed, address-ordered doubly linked list of physical
 * memory regions.  It is intended for the early boot phase, before the buddy
 * allocator and slab allocator are available.  The global pool is statically
 * initialized; callers add usable memory with mchain_add(), reserve ranges
 * with mchain_reserve(), and allocate with mchain_alloc().
 */

#ifndef BRK_MCHAIN_H
#define BRK_MCHAIN_H

#include <arch/asm/page.h>
#include <brk/types.h>
#include <stdbool.h>
#include <stddef.h>

/** @brief Number of entries in the initial static arena. */
#ifndef MCHAIN_STATIC_CAPACITY
#define MCHAIN_STATIC_CAPACITY 256
#endif

/** @brief Arena growth multiplier. */
#define MCHAIN_GROW_FACTOR 2

/** @brief Extra slots added during growth for the arena's own entry. */
#define MCHAIN_GROW_HEADROOM 2

/** @brief Physical alignment of a grown arena. */
#define MCHAIN_ARENA_ALIGN PAGE_SIZE

/* Region state bits: mutually exclusive, stored in the low four bits. */
#define MCHAIN_STATE_FREE 0x0u
#define MCHAIN_STATE_RESERVED 0x1u
#define MCHAIN_STATE_ALLOCATED 0x2u
#define MCHAIN_STATE_MASK 0xfu

/* Region attribute bits: combinable, stored above the state bits. */
#define MCHAIN_ATTR_HOTPLUG (1u << 4)
#define MCHAIN_ATTR_NOMAP (1u << 5)
#define MCHAIN_ATTR_MIRROR (1u << 6)
#define MCHAIN_ATTR_DRIVER (1u << 7)
#define MCHAIN_ATTR_MASK 0xfffffff0u

/** @brief NUMA node wildcard. */
#define MCHAIN_NID_ANY (-1)

/** @brief Address wildcard: use the global limit / no bound. */
#define MCHAIN_ANY_ADDR ((phys_addr_t) ~(phys_addr_t)0)

/** @brief Named-reservation table size, including the unnamed slot 0. */
#define MCHAIN_NAME_SLOTS 32

/** @brief Maximum reservation name length, including the terminating NUL. */
#define MCHAIN_NAME_MAX 32

/** @brief Reserved name id for unnamed reservations. */
#define MCHAIN_NAME_NONE 0u

/**
 * @brief One physical memory region.
 *
 * The region is the half-open range [base, base + size).  next/prev are
 * indices into the pool arena; -1 terminates the chain.
 */
struct mchain_entry {
	/** @brief Physical base address. */
	phys_addr_t base;

	/** @brief Region size in bytes. */
	phys_size_t size;

	/** @brief Next region in the address-ordered chain, or -1. */
	int next;

	/** @brief Previous region in the address-ordered chain, or -1. */
	int prev;

	/** @brief NUMA node, or MCHAIN_NID_ANY. */
	int nid;

	/** @brief MCHAIN_STATE_* | MCHAIN_ATTR_*. */
	unsigned int flags;

	/** @brief Named-reservation id, or MCHAIN_NAME_NONE. */
	unsigned int name_id;
};

/**
 * @brief mchain allocator state.
 *
 * Public APIs operate on mchain_pool_instance.  Internal helpers take a pool
 * pointer so that unit tests can use a separate pool.
 */
struct mchain_pool {
	/** @brief Current arena. */
	struct mchain_entry *entries;

	/** @brief Number of entries in the arena. */
	unsigned int capacity;

	/** @brief Number of entries currently in the region chain. */
	unsigned int used;

	/** @brief First region in the address-ordered chain, or -1. */
	int head;

	/** @brief First free arena entry, or -1 if not built / empty. */
	int free_head;

	/** @brief true for bottom-up allocation, false for top-down. */
	bool bottom_up;

	/** @brief Non-zero when arena growth is allowed. */
	int can_grow;

	/** @brief Non-zero after mchain_freeze(). */
	int frozen;

	/** @brief Non-zero after the static free list has been built. */
	int free_list_ready;

	/** @brief Global allocation lower bound. */
	phys_addr_t min_limit;

	/** @brief Global allocation upper bound. */
	phys_addr_t max_limit;

	/** @brief Peak value of used. */
	unsigned int peak_used;

	/** @brief Number of successful arena growths. */
	unsigned int grow_count;

	/** @brief Number of failed mchain_alloc() calls. */
	unsigned int alloc_failures;
};

/** @brief Global allocator instance. */
extern struct mchain_pool mchain_pool_instance;

/** @brief Free-memory iterator state. */
struct mchain_iter {
	/** @brief Current chain index, internal to the iterator. */
	int cur;

	/** @brief Inclusive lower bound. */
	phys_addr_t min_addr;

	/** @brief Exclusive upper bound. */
	phys_addr_t max_addr;

	/** @brief NUMA node filter, or MCHAIN_NID_ANY. */
	int nid;

	/** @brief Required attribute mask. */
	unsigned int attr_mask;
};

/** @brief Allocator statistics. */
struct mchain_stats {
	unsigned int used_entries;
	unsigned int peak_used_entries;
	unsigned int capacity;
	unsigned int grow_count;
	unsigned int alloc_failures;
	unsigned int frozen;
	phys_size_t free_bytes;
	phys_size_t reserved_bytes;
	phys_size_t allocated_bytes;
};

/* ------------------------------------------------------------------------- */
/* Lifecycle and allocation direction                                        */
/* ------------------------------------------------------------------------- */

int mchain_set_limit(phys_addr_t min_addr, phys_addr_t max_addr);
void mchain_set_bottom_up(bool bottom_up);
bool mchain_bottom_up(void);
void mchain_allow_grow(void);
void mchain_freeze(void);

/* ------------------------------------------------------------------------- */
/* Region management                                                        */
/* ------------------------------------------------------------------------- */

int mchain_add(phys_addr_t base, phys_size_t size, unsigned int attrs, int nid);
int mchain_reserve(phys_addr_t base, phys_size_t size, unsigned int attrs,
		   int nid);
int mchain_unreserve(phys_addr_t base, phys_size_t size);

int mchain_reserve_named(const char *name, phys_addr_t base, phys_size_t size,
			 unsigned int attrs, int nid);
int mchain_find_reserved(const char *name, phys_addr_t *base,
			 phys_size_t *size);

/* ------------------------------------------------------------------------- */
/* Allocation                                                               */
/* ------------------------------------------------------------------------- */

phys_addr_t mchain_find_in_range(phys_size_t size, phys_size_t align,
				 phys_addr_t min_addr, phys_addr_t max_addr,
				 int nid, unsigned int attr_mask);
phys_addr_t mchain_alloc(phys_size_t size, phys_size_t align,
			 phys_addr_t min_addr, phys_addr_t max_addr, int nid,
			 unsigned int attr_mask);
int mchain_free(phys_addr_t base, phys_size_t size);

/* ------------------------------------------------------------------------- */
/* Queries                                                                  */
/* ------------------------------------------------------------------------- */

bool mchain_range_is_free(phys_addr_t base, phys_size_t size);
bool mchain_is_memory(phys_addr_t addr);
phys_size_t mchain_free_size(void);
phys_size_t mchain_reserved_size(void);
phys_size_t mchain_memory_size(void);
phys_addr_t mchain_start_of_dram(void);
phys_addr_t mchain_end_of_dram(void);
unsigned int mchain_used_entries(void);
unsigned int mchain_capacity(void);

/* ------------------------------------------------------------------------- */
/* Iteration                                                                */
/* ------------------------------------------------------------------------- */

void mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr,
		      phys_addr_t max_addr, int nid, unsigned int attr_mask);
int mchain_iter_next(struct mchain_iter *it, phys_addr_t *start,
		     phys_addr_t *end);

/* ------------------------------------------------------------------------- */
/* Debug                                                                    */
/* ------------------------------------------------------------------------- */

int mchain_validate(void);
void mchain_dump(void);
void mchain_get_stats(struct mchain_stats *stats);

/* ------------------------------------------------------------------------- */
/* Internal helpers (for unit tests and mchain.c only)                      */
/* ------------------------------------------------------------------------- */

void mchain_bootstrap(struct mchain_pool *pool);
int mchain_ensure_entries(struct mchain_pool *pool, unsigned int nr,
			  phys_addr_t avoid_start, phys_addr_t avoid_end);
int mchain_insert_entry(struct mchain_pool *pool, phys_addr_t base,
			phys_size_t size, unsigned int state,
			unsigned int attrs, int nid, unsigned int name_id);
int mchain_carve_range(struct mchain_pool *pool, phys_addr_t start,
		       phys_addr_t end, unsigned int state,
		       unsigned int attr_mask, int nid, phys_size_t *carved);
int mchain_grow_pool(struct mchain_pool *pool, phys_addr_t avoid_start,
		     phys_addr_t avoid_end);
void mchain_unlink_entry(struct mchain_pool *pool, int idx);
int mchain_merge_neighbors(struct mchain_pool *pool, int idx);

#endif
