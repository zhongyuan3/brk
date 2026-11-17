/**
 * @file brk/memblock.h
 * @brief Boot-time physical memory allocator.
 *
 * Memblock manages physical memory before the buddy allocator and slab
 * allocator are available.  It tracks two collections of sorted,
 * non-overlapping regions:
 *
 * - @c memory describes physical memory available to the kernel.
 * - @c reserved describes ranges that have been reserved or allocated.
 *
 * A region represents the half-open physical range [base, base + size).
 * Adjacent regions with identical flags are merged.  Memory available for
 * allocation is the set difference @c memory @c - @c reserved.
 *
 * This is a simplified single-node version of the Linux memblock API.  It
 * keeps the same core semantics for the subset it provides.
 */

#ifndef BRK_MEMBLOCK_H
#define BRK_MEMBLOCK_H

#include <brk/types.h>
#include <stdbool.h>
#include <stddef.h>

/** @brief Initial number of static memory region entries. */
#define INIT_MEMBLOCK_MEMORY_REGIONS 128

/** @brief Initial number of static reserved region entries. */
#define INIT_MEMBLOCK_RESERVED_REGIONS 128

/**
 * @brief Memory region attributes.
 */
enum memblock_flags {
	/** @brief No special request. */
	MEMBLOCK_NONE = 0,

	/** @brief Memory that may be hot-unplugged later. */
	MEMBLOCK_HOTPLUG = 1 << 0,

	/** @brief Mirrored memory region. */
	MEMBLOCK_MIRROR = 1 << 1,

	/** @brief Do not add this region to the kernel direct map. */
	MEMBLOCK_NOMAP = 1 << 2,
};

/**
 * @brief Special allocation upper bounds.
 *
 * @c MEMBLOCK_ALLOC_ANYWHERE means that the allocation has no upper bound.
 *
 * @c MEMBLOCK_ALLOC_ACCESSIBLE means that the allocation is limited by
 * @ref memblock.current_limit.  It is only meaningful as the end of an
 * allocation range.
 */
#define MEMBLOCK_ALLOC_ANYWHERE UINT64_MAX
#define MEMBLOCK_ALLOC_ACCESSIBLE 0

/**
 * @brief One physical memory region.
 *
 * A region represents the half-open physical range
 * [@ref base, @ref base + @ref size).  Regions in a @ref memblock_type are
 * sorted by @ref base, never overlap, and are minimally merged.
 */
struct memblock_region {
	/** @brief Physical base address. */
	phys_addr_t base;

	/** @brief Region size in bytes. */
	phys_size_t size;

	/** @brief Region attributes, see @ref memblock_flags. */
	enum memblock_flags flags;
};

/**
 * @brief Collection of memory regions of one type.
 *
 * The region array starts as a static array and may be replaced by a larger
 * dynamically allocated array after @ref memblock_allow_resize() is called.
 */
struct memblock_type {
	/** @brief Symbolic name, such as "memory" or "reserved". */
	const char *name;

	/** @brief Array of region descriptors. */
	struct memblock_region *regions;

	/** @brief Number of entries allocated in @ref regions. */
	size_t max;

	/** @brief Number of entries currently in use. */
	size_t cnt;

	/** @brief Total size of all regions in bytes. */
	phys_size_t total_size;
};

/**
 * @brief Memblock allocator state.
 */
struct memblock {
	/** @brief Allocate from low addresses first when true. */
	bool bottom_up;

	/** @brief Upper bound used by @ref MEMBLOCK_ALLOC_ACCESSIBLE. */
	phys_addr_t current_limit;

	/** @brief Physical memory available to the kernel. */
	struct memblock_type memory;

	/** @brief Memory that has been reserved or allocated. */
	struct memblock_type reserved;
};

/** @brief Global memblock allocator state. */
extern struct memblock memblock;

/**
 * @brief Add a physical memory range to the memory type.
 *
 * The range [@p base, @p base + @p size) is added to @c memblock.memory.
 * The range may overlap existing memory regions; overlaps do not modify
 * existing regions.  The resulting region set is kept sorted, non-overlapping
 * and minimally merged.
 *
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return 0 on success, negative errno on failure.
 */
int memblock_add(phys_addr_t base, phys_size_t size);

/**
 * @brief Reserve a physical memory range.
 *
 * The range [@p base, @p base + @p size) is added to @c memblock.reserved.
 * Reserved ranges are excluded from subsequent memblock allocations.
 *
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return 0 on success, negative errno on failure.
 */
int memblock_reserve(phys_addr_t base, phys_size_t size);

/**
 * @brief Remove a range from the memory type.
 *
 * The range [@p base, @p base + @p size) is removed from @c memblock.memory.
 * This is used to punch holes into the physical memory map; the reserved type
 * is not modified.
 *
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return 0 on success, negative errno on failure.
 */
int memblock_remove(phys_addr_t base, phys_size_t size);

/**
 * @brief Enable automatic region-array growth.
 *
 * Enable automatic growth of the memory and reserved region arrays when they
 * run out of space.
 *
 * This must only be called after the final direct map is active, because new
 * arrays are accessed through @c phys_to_virt().  It is called from
 * @c paging_init() after @c load_final_page_table().  Before that point, the
 * static initial arrays must be large enough for boot-time reservations.
 */
void memblock_allow_resize(void);

/**
 * @brief Free a physical range previously reserved by memblock.
 *
 * Before the buddy allocator is available, this removes the range from
 * @c memblock.reserved.  After @ref memblock_free_all() has run, the pages are
 * returned directly to the buddy allocator instead.
 *
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return 0 on success, negative errno on failure.
 */
int memblock_phys_free(phys_addr_t base, phys_size_t size);

/**
 * @brief Set or clear region flags for a physical range.
 *
 * The range is isolated first, so regions crossing either boundary are split.
 * Neighbouring compatible regions are merged after the flags change.
 *
 * @param[in,out] type Memblock type to modify.
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 * @param[in] set Non-zero to set the flags, zero to clear them.
 * @param[in] flag Flags to modify, see @ref memblock_flags.
 *
 * @return 0 on success, negative errno on failure.
 */
int memblock_setclr_flag(struct memblock_type *type, phys_addr_t base,
			 phys_size_t size, int set, int flag);

/**
 * @brief Mark a memory range as not directly mapped.
 *
 * NOMAP memory still has struct pages and can remain in the memory map, but it
 * must not be used for ordinary direct-mapped allocations.  Free-memory
 * iterators skip it unless @ref MEMBLOCK_NOMAP is explicitly requested.
 *
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return 0 on success, negative errno on failure.
 */
int memblock_mark_nomap(phys_addr_t base, phys_size_t size);

/**
 * @brief Clear the NOMAP flag for a memory range.
 *
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return 0 on success, negative errno on failure.
 */
int memblock_clear_nomap(phys_addr_t base, phys_size_t size);

/**
 * @brief Test whether a region has the NOMAP flag.
 *
 * @param[in] rgn Region to test.
 *
 * @return true if @p rgn is marked @ref MEMBLOCK_NOMAP.
 */
static inline bool memblock_is_nomap(const struct memblock_region *rgn)
{
	return rgn->flags & MEMBLOCK_NOMAP;
}

/**
 * @brief Find a free physical range.
 *
 * Find, but do not reserve, a free physical range inside [@p start, @p end).
 * The search direction is controlled by @c memblock.bottom_up.
 *
 * @param[in] start Lower bound of the candidate range.
 * @param[in] end Upper bound, or @ref MEMBLOCK_ALLOC_ACCESSIBLE to use
 *                @c memblock.current_limit.
 * @param[in] size Size of the free range to find.
 * @param[in] align Alignment of the returned address; must be a power of two.
 *
 * @return Physical address on success, 0 on failure.
 */
phys_addr_t memblock_find_in_range(phys_addr_t start, phys_addr_t end,
				   phys_size_t size, phys_size_t align);

/**
 * @brief Allocate physical memory.
 *
 * Allocate memory below @c memblock.current_limit and reserve it.  This is the
 * physical-address variant of the Linux memblock allocator.
 *
 * @param[in] size Allocation size in bytes.
 * @param[in] align Alignment of the returned address; must be a power of two.
 *
 * @return Physical address on success, 0 on failure.
 */
phys_addr_t memblock_phys_alloc(phys_size_t size, phys_size_t align);

/**
 * @brief Allocate physical memory inside a range.
 *
 * Find a free range using @ref memblock_find_in_range() and reserve it.
 *
 * @param[in] size Allocation size in bytes.
 * @param[in] align Alignment of the returned address; must be a power of two.
 * @param[in] start Lower bound of the allocation range.
 * @param[in] end Upper bound, or @ref MEMBLOCK_ALLOC_ACCESSIBLE to use
 *                @c memblock.current_limit.
 *
 * @return Physical address on success, 0 on failure.
 */
phys_addr_t memblock_phys_alloc_range(phys_size_t size, phys_size_t align,
				      phys_addr_t start, phys_addr_t end);

/**
 * @brief Return the base of the first memory region.
 *
 * @return Physical base address of the first memory region, or 0 if the memory
 *         type is empty.
 */
phys_addr_t memblock_start_of_dram(void);

/**
 * @brief Return the end of the last memory region.
 *
 * @return End address of the highest memory region, or 0 if the memory type is
 *         empty.
 */
phys_addr_t memblock_end_of_dram(void);

/**
 * @brief Return the total size of the memory type.
 *
 * @return Total size of all memory regions in bytes.
 */
phys_size_t memblock_memory_size(void);

/**
 * @brief Return the total size of the reserved type.
 *
 * @return Total size of all reserved regions in bytes.
 */
phys_size_t memblock_reserved_size(void);

/**
 * @brief Test whether an address belongs to memory.
 *
 * @param[in] addr Physical address.
 *
 * @return true if @p addr is covered by a memory region.
 */
bool memblock_is_memory(phys_addr_t addr);

/**
 * @brief Test whether an address belongs to reserved memory.
 *
 * @param[in] addr Physical address.
 *
 * @return true if @p addr is covered by a reserved region.
 */
bool memblock_is_reserved(phys_addr_t addr);

/**
 * @brief Test whether a range is a subset of memory.
 *
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return true if the range is fully contained in a single memory region.
 */
bool memblock_is_region_memory(phys_addr_t base, phys_size_t size);

/**
 * @brief Test whether a range intersects reserved memory.
 *
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return true if the range intersects any reserved region.
 */
bool memblock_is_region_reserved(phys_addr_t base, phys_size_t size);

/**
 * @brief Test whether a range overlaps a memblock type.
 *
 * @param[in] type Memblock type to check.
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return true if the range overlaps any region in @p type.
 */
bool memblock_overlaps_region(const struct memblock_type *type,
			      phys_addr_t base, phys_size_t size);

/**
 * @brief Set the accessible allocation limit.
 *
 * This limits allocations that use @ref MEMBLOCK_ALLOC_ACCESSIBLE.  It is used
 * when only part of physical memory is currently addressable during boot.
 *
 * @param[in] limit New physical allocation limit.
 */
void memblock_set_current_limit(phys_addr_t limit);

/**
 * @brief Read the accessible allocation limit.
 *
 * @return Current physical allocation limit.
 */
phys_addr_t memblock_get_current_limit(void);

/**
 * @brief Query the allocation direction.
 *
 * @return true when memblock allocates from low addresses first.
 */
bool memblock_bottom_up(void);

/**
 * @brief Select the allocation direction.
 *
 * @param[in] bottom_up true for bottom-up allocation, false for top-down.
 */
void memblock_set_bottom_up(bool bottom_up);

/**
 * @brief Validate memblock internal invariants.
 *
 * Check region ordering, overlap, merging, total sizes and address overflow.
 * This is a BRK-specific debugging helper, not a Linux public API.
 *
 * @return 0 when both memblock types are consistent, negative errno otherwise.
 */
int memblock_validate(void);

/**
 * @brief Release free memory to the buddy allocator.
 *
 * Release every free (memory && !reserved) range to the buddy allocator.
 * Afterwards memblock allocation is disabled and automatic resizing is turned
 * off.  Calling this function more than once has no effect.
 */
void memblock_free_all(void);

/**
 * @brief Iterate over type_a ranges not covered by type_b.
 *
 * This is the generic iterator behind the for_each_* range macros.  It uses
 * the same packed-index scheme as Linux memblock: the low 32 bits index
 * @p type_a and the high 32 bits index @p type_b.  UINT64_MAX signals the end
 * of iteration.
 *
 * NOMAP memory is skipped unless @ref MEMBLOCK_NOMAP is present in @p flags.
 *
 * @param[in,out] pidx Iterator state.
 * @param[in] type_a Regions to iterate.
 * @param[in] type_b Regions to exclude; may be NULL.
 * @param[in] flags Region flags to include explicitly.
 * @param[out] pstart Output start address; may be NULL.
 * @param[out] pend Output end address; may be NULL.
 */
void __next_mem_range(u64 *pidx, const struct memblock_type *type_a,
		      const struct memblock_type *type_b,
		      enum memblock_flags flags, phys_addr_t *pstart,
		      phys_addr_t *pend);

/**
 * @brief Reverse iterate over type_a ranges not covered by type_b.
 *
 * Reverse counterpart of @ref __next_mem_range().  UINT64_MAX initializes the
 * reverse walk and also signals the end of iteration.
 *
 * @param[in,out] pidx Iterator state.
 * @param[in] type_a Regions to iterate.
 * @param[in] type_b Regions to exclude; may be NULL.
 * @param[in] flags Region flags to include explicitly.
 * @param[out] pstart Output start address; may be NULL.
 * @param[out] pend Output end address; may be NULL.
 */
void __next_mem_range_rev(u64 *pidx, const struct memblock_type *type_a,
			  const struct memblock_type *type_b,
			  enum memblock_flags flags, phys_addr_t *pstart,
			  phys_addr_t *pend);

/**
 * @brief Iterate over configured memory PFN ranges.
 *
 * Walk over all configured memory ranges, including reserved and NOMAP
 * memory.  This is the low-level helper for @ref for_each_mem_pfn_range().
 *
 * @param[in,out] pidx Iterator index; UINT64_MAX signals the end.
 * @param[out] pstart Output start PFN; may be NULL.
 * @param[out] pend Output end PFN; may be NULL.
 */
void __next_mem_pfn_range(u64 *pidx, pfn_t *pstart, pfn_t *pend);

/**
 * @brief Generic typed memblock range iterator.
 *
 * @param[in,out] idx u64 loop variable.
 * @param[in] type_a Regions to iterate.
 * @param[in] type_b Regions to exclude; may be NULL.
 * @param[in] flags Region flags to include explicitly.
 * @param[out] start Start address output variable.
 * @param[out] end End address output variable.
 */
#define __for_each_mem_range(idx, type_a, type_b, flags, start, end)           \
	for ((idx) = 0, __next_mem_range(&(idx), (type_a), (type_b), (flags),  \
					 &(start), &(end));                    \
	     (idx) != UINT64_MAX; __next_mem_range(&(idx), (type_a), (type_b), \
						   (flags), &(start), &(end)))

/**
 * @brief Generic reverse typed memblock range iterator.
 *
 * @param[in,out] idx u64 loop variable.
 * @param[in] type_a Regions to iterate.
 * @param[in] type_b Regions to exclude; may be NULL.
 * @param[in] flags Region flags to include explicitly.
 * @param[out] start Start address output variable.
 * @param[out] end End address output variable.
 */
#define __for_each_mem_range_rev(idx, type_a, type_b, flags, start, end) \
	for ((idx) = UINT64_MAX,                                         \
	    __next_mem_range_rev(&(idx), (type_a), (type_b), (flags),    \
				 &(start), &(end));                      \
	     (idx) != UINT64_MAX; __next_mem_range_rev(                  \
		     &(idx), (type_a), (type_b), (flags), &(start), &(end)))

/**
 * @brief Iterate over memory ranges.
 *
 * Iterate over configured memory regions, excluding NOMAP memory by default.
 *
 * @param[in,out] idx u64 loop variable.
 * @param[out] start Start address output variable.
 * @param[out] end End address output variable.
 */
#define for_each_mem_range(idx, start, end)                              \
	__for_each_mem_range(idx, &memblock.memory, NULL, MEMBLOCK_NONE, \
			     start, end)

/**
 * @brief Reverse iterate over memory ranges.
 *
 * @param[in,out] idx u64 loop variable.
 * @param[out] start Start address output variable.
 * @param[out] end End address output variable.
 */
#define for_each_mem_range_rev(idx, start, end)                              \
	__for_each_mem_range_rev(idx, &memblock.memory, NULL, MEMBLOCK_NONE, \
				 start, end)

/**
 * @brief Iterate over free memory ranges.
 *
 * Iterate over @c memory @c - @c reserved ranges.  NOMAP memory is excluded
 * unless explicitly requested by the generic iterator.
 *
 * @param[in,out] idx u64 loop variable.
 * @param[out] start Start address output variable.
 * @param[out] end End address output variable.
 */
#define for_each_free_mem_range(idx, start, end)                        \
	__for_each_mem_range(idx, &memblock.memory, &memblock.reserved, \
			     MEMBLOCK_NONE, start, end)

/**
 * @brief Reverse iterate over free memory ranges.
 *
 * @param[in,out] idx u64 loop variable.
 * @param[out] start Start address output variable.
 * @param[out] end End address output variable.
 */
#define for_each_free_mem_range_rev(idx, start, end)                        \
	__for_each_mem_range_rev(idx, &memblock.memory, &memblock.reserved, \
				 MEMBLOCK_NONE, start, end)

/**
 * @brief Iterate over reserved memory ranges.
 *
 * @param[in,out] idx u64 loop variable.
 * @param[out] start Start address output variable.
 * @param[out] end End address output variable.
 */
#define for_each_reserved_mem_range(idx, start, end)                       \
	__for_each_mem_range(idx, &memblock.reserved, NULL, MEMBLOCK_NONE, \
			     start, end)

/**
 * @brief Iterate over raw memory region descriptors.
 *
 * Unlike the range iterators, this exposes every memory region including
 * NOMAP memory.
 *
 * @param[in,out] rgn @ref memblock_region loop variable.
 */
#define for_each_mem_region(rgn)              \
	for ((rgn) = memblock.memory.regions; \
	     (rgn) < memblock.memory.regions + memblock.memory.cnt; (rgn)++)

/**
 * @brief Iterate over raw reserved region descriptors.
 *
 * @param[in,out] rgn @ref memblock_region loop variable.
 */
#define for_each_reserved_mem_region(rgn)                               \
	for ((rgn) = memblock.reserved.regions;                         \
	     (rgn) < memblock.reserved.regions + memblock.reserved.cnt; \
	     (rgn)++)

/**
 * @brief Iterate over configured memory PFN ranges.
 *
 * @param[in,out] idx u64 loop variable.
 * @param[out] start Start PFN output variable.
 * @param[out] end End PFN output variable.
 */
#define for_each_mem_pfn_range(idx, start, end)                         \
	for ((idx) = 0, __next_mem_pfn_range(&(idx), &(start), &(end)); \
	     (idx) != UINT64_MAX;                                       \
	     __next_mem_pfn_range(&(idx), &(start), &(end)))

#endif
