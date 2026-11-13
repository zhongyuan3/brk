#ifndef BRK_MEMBLOCK_H
#define BRK_MEMBLOCK_H

#include <brk/types.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Simplified memblock implementation: single NUMA node and no region flags
 * yet.  Names and semantics follow the Linux boot-time memory API for the
 * subset implemented here.
 */

#define INIT_MEMBLOCK_MEMORY_REGIONS 128
#define INIT_MEMBLOCK_RESERVED_REGIONS 128

/*
 * Special end values for memblock_find_in_range() and the physical allocator.
 * MEMBLOCK_ALLOC_ANYWHERE means no upper bound; MEMBLOCK_ALLOC_ACCESSIBLE
 * means "use memblock.current_limit".
 */
#define MEMBLOCK_ALLOC_ANYWHERE UINT64_MAX
#define MEMBLOCK_ALLOC_ACCESSIBLE 0

struct memblock_region {
	u64 base;
	u64 size;
};

struct memblock_type {
	const char *name;
	struct memblock_region *regions;
	size_t max;
	size_t cnt;
	u64 total_size;
};

struct memblock {
	bool bottom_up;
	u64 current_limit;
	struct memblock_type memory;
	struct memblock_type reserved;
};

extern struct memblock memblock;

int memblock_add(u64 base, u64 size);
int memblock_reserve(u64 base, u64 size);
int memblock_remove(u64 base, u64 size);
/* Release a physical range previously reserved by memblock. */
int memblock_phys_free(u64 base, u64 size);

/* Find a free physical range without reserving it. */
u64 memblock_find_in_range(u64 start, u64 end, u64 size, u64 align);
/* Allocate physical memory below memblock.current_limit. */
u64 memblock_phys_alloc(u64 size, u64 align);
/*
 * Allocate physical memory from [start, end).  Pass
 * MEMBLOCK_ALLOC_ACCESSIBLE as end to use memblock.current_limit.
 */
u64 memblock_phys_alloc_range(u64 size, u64 align, u64 start, u64 end);

u64 memblock_start_of_dram(void);
u64 memblock_end_of_dram(void);
u64 memblock_memory_size(void);
u64 memblock_reserved_size(void);

bool memblock_is_memory(u64 addr);
bool memblock_is_reserved(u64 addr);
bool memblock_is_region_memory(u64 base, u64 size);
bool memblock_is_region_reserved(u64 base, u64 size);
bool memblock_overlaps_region(const struct memblock_type *type, u64 base,
			      u64 size);

void memblock_set_current_limit(u64 limit);
u64 memblock_get_current_limit(void);
bool memblock_bottom_up(void);
void memblock_set_bottom_up(bool bottom_up);

int memblock_validate(void);
void memblock_free_all(void);

void __next_mem_range(u64 *pidx, const struct memblock_type *type_a,
		      const struct memblock_type *type_b, u64 *pstart,
		      u64 *pend);
void __next_mem_range_rev(u64 *pidx, const struct memblock_type *type_a,
			  const struct memblock_type *type_b, u64 *pstart,
			  u64 *pend);
void __next_mem_pfn_range(u64 *pidx, u64 *pstart, u64 *pend);

#define __for_each_mem_range(idx, type_a, type_b, start, end)               \
	for ((idx) = 0,                                                     \
	    __next_mem_range(&(idx), (type_a), (type_b), &(start), &(end)); \
	     (idx) != UINT64_MAX;                                           \
	     __next_mem_range(&(idx), (type_a), (type_b), &(start), &(end)))

#define __for_each_mem_range_rev(idx, type_a, type_b, start, end)      \
	for ((idx) = UINT64_MAX,                                       \
	    __next_mem_range_rev(&(idx), (type_a), (type_b), &(start), \
				 &(end));                              \
	     (idx) != UINT64_MAX; __next_mem_range_rev(                \
		     &(idx), (type_a), (type_b), &(start), &(end)))

/* Iterate over all memory regions. */
#define for_each_mem_range(idx, start, end) \
	__for_each_mem_range(idx, &memblock.memory, NULL, start, end)

#define for_each_mem_range_rev(idx, start, end) \
	__for_each_mem_range_rev(idx, &memblock.memory, NULL, start, end)

/* Iterate over free (memory && !reserved) regions. */
#define for_each_free_mem_range(idx, start, end)                               \
	__for_each_mem_range(idx, &memblock.memory, &memblock.reserved, start, \
			     end)

#define for_each_free_mem_range_rev(idx, start, end)                        \
	__for_each_mem_range_rev(idx, &memblock.memory, &memblock.reserved, \
				 start, end)

/* Iterate over reserved regions. */
#define for_each_reserved_mem_range(idx, start, end) \
	__for_each_mem_range(idx, &memblock.reserved, NULL, start, end)

#define for_each_mem_region(rgn)              \
	for ((rgn) = memblock.memory.regions; \
	     (rgn) < memblock.memory.regions + memblock.memory.cnt; (rgn)++)

#define for_each_reserved_mem_region(rgn)                               \
	for ((rgn) = memblock.reserved.regions;                         \
	     (rgn) < memblock.reserved.regions + memblock.reserved.cnt; \
	     (rgn)++)

#define for_each_mem_pfn_range(idx, start, end)                         \
	for ((idx) = 0, __next_mem_pfn_range(&(idx), &(start), &(end)); \
	     (idx) != UINT64_MAX;                                       \
	     __next_mem_pfn_range(&(idx), &(start), &(end)))

#endif
