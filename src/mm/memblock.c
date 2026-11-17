/**
 * @file mm/memblock.c
 * @brief Boot-time physical memory allocator.
 *
 * The memory and reserved arrays start in .bss.  Once the final direct map is
 * active, @ref memblock_allow_resize() permits them to be replaced by larger
 * arrays allocated from memblock itself.
 */

#include <arch/asm/page.h>
#include <brk/align.h>
#include <brk/assert.h>
#include <brk/log2.h>
#include <brk/memblock.h>
#include <brk/min_max.h>
#include <brk/pgalloc.h>
#include <brk/string.h>
#include <stdbool.h>
#include <uapi/errno.h>

#define for_each_memblock_region(set, idx, rgn)               \
	for (idx = 0, rgn = &set->regions[0]; idx < set->cnt; \
	     ++idx, rgn = &set->regions[idx])

static struct memblock_region memory[INIT_MEMBLOCK_MEMORY_REGIONS];
static struct memblock_region reserved[INIT_MEMBLOCK_RESERVED_REGIONS];
static bool memblock_alloc_enabled = true;
static bool memblock_free_all_done;
static bool memblock_can_resize;

static void memblock_free_to_buddy(u64 start, u64 end);

struct memblock memblock = {
	.bottom_up = true,
	.current_limit = MEMBLOCK_ALLOC_ANYWHERE,
	.memory.name = "memory",
	.memory.regions = memory,
	.memory.max = INIT_MEMBLOCK_MEMORY_REGIONS,
	.memory.cnt = 0,
	.memory.total_size = 0,
	.reserved.name = "reserved",
	.reserved.regions = reserved,
	.reserved.max = INIT_MEMBLOCK_RESERVED_REGIONS,
	.reserved.cnt = 0,
	.reserved.total_size = 0,
};

static void memblock_move_regions(struct memblock_type *type, size_t to,
				  size_t from, size_t n)
{
	memmove(&type->regions[to], &type->regions[from],
		n * sizeof(struct memblock_region));
}

static int memblock_insert_region(struct memblock_type *type, size_t idx,
				  u64 base, u64 size, enum memblock_flags flags)
{
	if (!size || idx > type->cnt)
		return -EINVAL;
	if (type->cnt >= type->max)
		return -ENOMEM;
	if (type->total_size + size < type->total_size)
		return -EINVAL;

	if (idx < type->cnt)
		memblock_move_regions(type, idx + 1, idx, type->cnt - idx);

	type->regions[idx].base = base;
	type->regions[idx].size = size;
	type->regions[idx].flags = flags;
	type->cnt++;
	type->total_size += size;
	return 0;
}

static void memblock_remove_region_at(struct memblock_type *type, size_t idx)
{
	if (idx >= type->cnt)
		return;

	type->total_size -= type->regions[idx].size;
	if (idx + 1 < type->cnt)
		memblock_move_regions(type, idx, idx + 1, type->cnt - idx - 1);
	type->cnt--;
}

static void memblock_merge_regions(struct memblock_type *type)
{
	if (type->cnt < 2)
		return;

	size_t idx = 0;
	while (idx < type->cnt - 1) {
		struct memblock_region *this = &type->regions[idx];
		struct memblock_region *next = &type->regions[idx + 1];

		if (this->base + this->size == next->base &&
		    this->flags == next->flags) {
			this->size += next->size;
			memblock_move_regions(type, idx + 1, idx + 2,
					      type->cnt - idx - 2);
			type->cnt--;
		} else {
			idx++;
		}
	}
}

/**
 * @brief Validate one memblock type.
 *
 * Regions must be sorted, non-overlapping, have no address/size overflow, and
 * be minimally merged when their flags match.
 *
 * @param[in] type Memblock type to validate.
 *
 * @return 0 on success, negative errno on failure.
 */
static int memblock_validate_type(const struct memblock_type *type)
{
	u64 total_size = 0;

	if (type->cnt > type->max)
		return -EINVAL;

	for (size_t idx = 0; idx < type->cnt; ++idx) {
		const struct memblock_region *rgn = &type->regions[idx];

		if (!rgn->size)
			return -EINVAL;
		if (rgn->base + rgn->size < rgn->base)
			return -EINVAL;
		if (total_size + rgn->size < total_size)
			return -EINVAL;
		total_size += rgn->size;

		if (idx + 1 < type->cnt) {
			const struct memblock_region *next =
				&type->regions[idx + 1];

			/* Regions must be sorted and non-overlapping. */
			if (rgn->base + rgn->size > next->base)
				return -EINVAL;

			/* Neighbouring compatible regions must be merged. */
			if (rgn->base + rgn->size == next->base &&
			    rgn->flags == next->flags)
				return -EINVAL;
		}
	}

	if (total_size != type->total_size)
		return -EINVAL;

	return 0;
}

int memblock_validate(void)
{
	int err;

	err = memblock_validate_type(&memblock.memory);
	if (err)
		return err;

	return memblock_validate_type(&memblock.reserved);
}

void memblock_allow_resize(void)
{
	memblock_can_resize = true;
}

/**
 * @brief Grow a memblock type's region array.
 *
 * The array is allocated with @ref memblock_find_in_range() rather than
 * @ref memblock_phys_alloc() to avoid recursively reserving an entry in a full
 * reserved array.
 *
 * @p avoid_start and @p avoid_size describe a pending range that must not
 * overlap the new array.  This is used when the reserved array is grown to
 * reserve the new memory-array allocation itself.
 *
 * @param[in,out] type Memblock type whose array should grow.
 * @param[in] avoid_start Start of a range the new array must avoid.
 * @param[in] avoid_size Size of the range to avoid.
 *
 * @return 0 on success, negative errno on failure.
 */
static int memblock_grow_array(struct memblock_type *type, u64 avoid_start,
			       u64 avoid_size)
{
	struct memblock_region *old_regions = type->regions;
	struct memblock_region *new_regions;
	size_t old_max = type->max;
	size_t new_max;
	size_t old_bytes;
	size_t new_bytes;
	size_t alloc_bytes;
	u64 avoid_end;
	u64 new_pa = 0;
	int err;

	if (!memblock_can_resize || !memblock_alloc_enabled)
		return -ENOMEM;
	if (old_max > (SIZE_MAX - 1) / 2)
		return -ENOMEM;

	/*
	 * +1 guarantees enough free slots for reserving the new array even in
	 * the worst case where its range creates cnt + 1 new regions.
	 */
	new_max = old_max * 2 + 1;
	old_bytes = old_max * sizeof(*old_regions);
	new_bytes = new_max * sizeof(*new_regions);
	if (new_max && new_bytes / sizeof(*new_regions) != new_max)
		return -ENOMEM;

	alloc_bytes = align_up(new_bytes, PAGE_SIZE);
	if (alloc_bytes < new_bytes)
		return -ENOMEM;

	if (avoid_size) {
		if (avoid_start + avoid_size < avoid_start)
			return -EINVAL;
		avoid_end = avoid_start + avoid_size;

		if (avoid_start)
			new_pa = memblock_find_in_range(0, avoid_start,
							alloc_bytes, PAGE_SIZE);
		if (!new_pa && avoid_end < memblock.current_limit)
			new_pa = memblock_find_in_range(avoid_end,
							memblock.current_limit,
							alloc_bytes, PAGE_SIZE);
	} else {
		new_pa = memblock_find_in_range(0, memblock.current_limit,
						alloc_bytes, PAGE_SIZE);
	}

	if (!new_pa)
		return -ENOMEM;

	new_regions = (struct memblock_region *)phys_to_virt(new_pa);
	memset(new_regions, 0, alloc_bytes);
	memcpy(new_regions, old_regions, old_bytes);

	/*
	 * Growing reserved itself must install the larger array before
	 * reserving the new array, because the old reserved array may be full.
	 * Growing memory can reserve the new array first, since the memory
	 * type still has the old (valid) array.
	 */
	if (type == &memblock.reserved) {
		type->regions = new_regions;
		type->max = new_max;

		err = memblock_reserve(new_pa, alloc_bytes);
		if (err) {
			type->regions = old_regions;
			type->max = old_max;
			return err;
		}
	} else {
		err = memblock_reserve(new_pa, alloc_bytes);
		if (err)
			return err;

		type->regions = new_regions;
		type->max = new_max;
	}

	if (old_regions != memory && old_regions != reserved) {
		u64 old_pa = virt_to_phys((uintptr_t)old_regions);
		size_t old_alloc_bytes = align_up(old_bytes, PAGE_SIZE);

		(void)memblock_phys_free(old_pa, old_alloc_bytes);
	}

	return 0;
}

/**
 * @brief Add a range to a memblock type.
 *
 * The new range may overlap existing regions.  Existing regions are never
 * modified; only the parts not already covered are inserted.  This keeps the
 * type sorted, non-overlapping and minimally merged, as required by the Linux
 * memblock API.
 *
 * The first pass counts how many new regions are needed.  If the region array
 * is too small, @ref memblock_grow_array() is called and the pass is repeated.
 *
 * @param[in,out] type Memblock type to modify.
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 * @param[in] flags Flags for newly inserted regions.
 *
 * @return 0 on success, negative errno on failure.
 */
static int memblock_add_region(struct memblock_type *type, u64 base, u64 size,
			       enum memblock_flags flags)
{
	size_t idx;
	size_t nr_new;
	struct memblock_region *rgn;
	u64 old_base = base;
	u64 end;
	bool insert = false;
	int err;

	if (!size)
		return 0;
	if (base + size < base)
		return -EINVAL;
	if (type->cnt > type->max)
		return -EINVAL;

	end = base + size;

repeat:
	base = old_base;
	nr_new = 0;

	for_each_memblock_region(type, idx, rgn) {
		u64 rbase = rgn->base;
		u64 rend = rbase + rgn->size;

		if (end <= rbase)
			break;

		if (base >= rend)
			continue;

		if (base < rbase) {
			nr_new++;
			if (insert) {
				err = memblock_insert_region(
					type, idx++, base, rbase - base, flags);
				if (err)
					return err;
			}
		}

		base = min(end, rend);
	}

	if (end > base) {
		nr_new++;
		if (insert) {
			err = memblock_insert_region(type, idx, base,
						     end - base, flags);
			if (err)
				return err;
		}
	}

	if (!insert) {
		while (nr_new > type->max - type->cnt) {
			err = memblock_grow_array(type, old_base, size);
			if (err)
				return err;
		}
		if (!nr_new)
			return 0;
		insert = true;
		goto repeat;
	}

	memblock_merge_regions(type);
	assert(memblock_validate() == 0);
	return 0;
}

int memblock_add(u64 base, u64 size)
{
	return memblock_add_region(&memblock.memory, base, size, MEMBLOCK_NONE);
}

int memblock_reserve(u64 base, u64 size)
{
	return memblock_add_region(&memblock.reserved, base, size,
				   MEMBLOCK_NONE);
}

/**
 * @brief Remove a range from a memblock type.
 *
 * Regions fully covered by the range are dropped; partial overlaps are
 * truncated or split.  The caller chooses whether this operates on memory or
 * reserved.
 *
 * @param[in,out] type Memblock type to modify.
 * @param[in] base Physical base address.
 * @param[in] size Range size in bytes.
 *
 * @return 0 on success, negative errno on failure.
 */
static int memblock_remove_range(struct memblock_type *type, u64 base, u64 size)
{
	size_t idx = 0;
	u64 end;
	int err;

	if (!size)
		return 0;
	if (base + size < base)
		return -EINVAL;
	if (type->cnt >= type->max) {
		err = memblock_grow_array(type, 0, 0);
		if (err)
			return err;
	}

	end = base + size;

	while (idx < type->cnt) {
		struct memblock_region *rgn = &type->regions[idx];
		u64 rbase = rgn->base;
		u64 rend = rbase + rgn->size;

		if (rend <= base) {
			idx++;
			continue;
		}
		if (rbase >= end)
			break;

		if (base <= rbase && end >= rend) {
			memblock_remove_region_at(type, idx);
			continue;
		}

		if (base <= rbase) {
			/* Trim the left edge: rbase < end < rend. */
			type->total_size -= end - rbase;
			rgn->base = end;
			rgn->size = rend - end;
			break;
		}

		if (end >= rend) {
			/* Trim the right edge: rbase < base < rend. */
			type->total_size -= rend - base;
			rgn->size = base - rbase;
			idx++;
			continue;
		}

		/* Split the region: rbase < base < end < rend. */
		err = memblock_insert_region(type, idx + 1, end, rend - end,
					     rgn->flags);
		if (err)
			return err;
		type->regions[idx].size = base - rbase;
		type->total_size -= end - base;
		break;
	}

	assert(memblock_validate() == 0);
	return 0;
}

int memblock_remove(u64 base, u64 size)
{
	return memblock_remove_range(&memblock.memory, base, size);
}

int memblock_phys_free(u64 base, u64 size)
{
	if (!size)
		return 0;
	if (base + size < base)
		return -EINVAL;

	if (memblock_free_all_done) {
		memblock_free_to_buddy(base, base + size);
		return 0;
	}

	return memblock_remove_range(&memblock.reserved, base, size);
}

/**
 * @brief Split a region at a given address.
 *
 * Preserve the region flags and keep @c total_size unchanged.  The right half
 * is inserted at @p idx + 1.
 *
 * @param[in,out] type Memblock type containing the region.
 * @param[in] idx Index of the region to split.
 * @param[in] addr Address at which to split.
 *
 * @return 0 on success, negative errno on failure.
 */
static int memblock_split_at(struct memblock_type *type, size_t idx, u64 addr)
{
	struct memblock_region *rgn = &type->regions[idx];
	u64 rbase = rgn->base;
	u64 rend = rbase + rgn->size;
	u64 right = rend - addr;
	int err;

	if (addr <= rbase || addr >= rend)
		return 0;

	err = memblock_insert_region(type, idx + 1, addr, right, rgn->flags);
	if (err)
		return err;

	/*
	 * Inserting the right part added it to total_size.  The original
	 * region's size is replaced by the left part, so subtract the right
	 * part again to keep total_size unchanged.
	 */
	type->total_size -= right;
	rgn->size = addr - rbase;
	return 0;
}

/**
 * @brief Isolate a range into disjoint memblock regions.
 *
 * Split all regions crossing the range boundaries and return the index range
 * of regions fully inside that range.  At most two extra regions are created.
 *
 * @param[in,out] type Memblock type to isolate the range in.
 * @param[in] base Range base address.
 * @param[in] size Range size in bytes.
 * @param[out] start_rgn Index of the first region inside the range.
 * @param[out] end_rgn Index of the first region after the range.
 *
 * @return 0 on success, negative errno on failure.
 */
static int memblock_isolate_range(struct memblock_type *type, u64 base,
				  u64 size, size_t *start_rgn, size_t *end_rgn)
{
	size_t first = type->cnt;
	size_t last = type->cnt;
	u64 end;
	int err;

	if (!size) {
		for (first = 0; first < type->cnt; ++first) {
			if (type->regions[first].base >= base)
				break;
		}
		*start_rgn = first;
		*end_rgn = first;
		return 0;
	}

	if (base + size < base)
		return -EINVAL;

	while (type->cnt + 2 > type->max) {
		err = memblock_grow_array(type, base, size);
		if (err)
			return err;
	}

	end = base + size;

	for (size_t idx = 0; idx < type->cnt; ++idx) {
		struct memblock_region *rgn = &type->regions[idx];
		u64 rend = rgn->base + rgn->size;

		if (rgn->base >= base)
			break;
		if (base < rend) {
			err = memblock_split_at(type, idx, base);
			if (err)
				return err;
			break;
		}
	}

	for (size_t idx = 0; idx < type->cnt; ++idx) {
		struct memblock_region *rgn = &type->regions[idx];
		u64 rend = rgn->base + rgn->size;

		if (rgn->base >= end)
			break;
		if (end < rend) {
			err = memblock_split_at(type, idx, end);
			if (err)
				return err;
			break;
		}
	}

	for (first = 0; first < type->cnt; ++first) {
		if (type->regions[first].base >= base)
			break;
	}
	for (last = first; last < type->cnt; ++last) {
		if (type->regions[last].base >= end)
			break;
	}

	*start_rgn = first;
	*end_rgn = last;
	return 0;
}

int memblock_setclr_flag(struct memblock_type *type, u64 base, u64 size,
			 int set, int flag)
{
	enum memblock_flags mask = (enum memblock_flags)flag;
	size_t start_rgn, end_rgn;
	int err;

	if (!size)
		return 0;
	if (base + size < base)
		return -EINVAL;

	err = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (err)
		return err;

	for (size_t idx = start_rgn; idx < end_rgn; ++idx) {
		if (set)
			type->regions[idx].flags |= mask;
		else
			type->regions[idx].flags &= ~mask;
	}

	memblock_merge_regions(type);
	assert(memblock_validate() == 0);
	return 0;
}

int memblock_mark_nomap(u64 base, u64 size)
{
	return memblock_setclr_flag(&memblock.memory, base, size, 1,
				    MEMBLOCK_NOMAP);
}

int memblock_clear_nomap(u64 base, u64 size)
{
	return memblock_setclr_flag(&memblock.memory, base, size, 0,
				    MEMBLOCK_NOMAP);
}

/**
 * @brief Find the region containing an address.
 *
 * @param[in] type Memblock type to search.
 * @param[in] addr Physical address.
 *
 * @return Matching region, or NULL for an address outside the type, including
 *         addresses in a memory hole.
 */
static const struct memblock_region *
memblock_search(const struct memblock_type *type, u64 addr)
{
	size_t left = 0;
	size_t right = type->cnt;

	while (left < right) {
		size_t mid = left + (right - left) / 2;
		const struct memblock_region *rgn = &type->regions[mid];

		if (addr < rgn->base)
			right = mid;
		else if (addr >= rgn->base + rgn->size)
			left = mid + 1;
		else
			return rgn;
	}

	return NULL;
}

/**
 * @brief Find a free range without reserving it.
 *
 * Allocation starts from the low or high end according to
 * @c memblock.bottom_up.
 *
 * @param[in] start Lower bound of the candidate range.
 * @param[in] end Upper bound, or @ref MEMBLOCK_ALLOC_ACCESSIBLE.
 * @param[in] size Size of the free range to find.
 * @param[in] align Alignment of the returned address.
 *
 * @return Physical address on success, 0 on failure.
 */
u64 memblock_find_in_range(u64 start, u64 end, u64 size, u64 align)
{
	u64 idx;
	u64 rstart, rend;

	if (end == MEMBLOCK_ALLOC_ACCESSIBLE)
		end = memblock.current_limit;

	if (!size || !is_power_of_2(align) || start >= end)
		return 0;

	if (memblock.bottom_up) {
		for_each_free_mem_range(idx, rstart, rend)
		{
			u64 range_start = max(rstart, start);
			u64 range_end = min(rend, end);
			u64 aligned;

			if (range_start >= range_end ||
			    size > range_end - range_start)
				continue;

			aligned = align_up(range_start, align);
			if (aligned < range_start)
				continue;
			if (aligned >= range_end || size > range_end - aligned)
				continue;
			return aligned;
		}
		return 0;
	}

	for_each_free_mem_range_rev(idx, rstart, rend)
	{
		u64 range_start = max(rstart, start);
		u64 range_end = min(rend, end);
		u64 aligned;

		if (range_start >= range_end || size > range_end - range_start)
			continue;

		aligned = align_down(range_end - size, align);
		if (aligned < range_start)
			continue;
		return aligned;
	}

	return 0;
}

u64 memblock_phys_alloc_range(u64 size, u64 align, u64 start, u64 end)
{
	u64 addr;

	if (!memblock_alloc_enabled)
		return 0;

	if (end == MEMBLOCK_ALLOC_ACCESSIBLE)
		end = memblock.current_limit;

	addr = memblock_find_in_range(start, end, size, align);
	if (!addr)
		return 0;
	if (memblock_reserve(addr, size))
		return 0;

	return addr;
}

u64 memblock_phys_alloc(u64 size, u64 align)
{
	return memblock_phys_alloc_range(size, align, 0,
					 MEMBLOCK_ALLOC_ACCESSIBLE);
}

u64 memblock_start_of_dram(void)
{
	if (!memblock.memory.cnt)
		return 0;

	return memblock.memory.regions[0].base;
}

u64 memblock_end_of_dram(void)
{
	if (!memblock.memory.cnt)
		return 0;

	const struct memblock_region *rgn =
		&memblock.memory.regions[memblock.memory.cnt - 1];

	return rgn->base + rgn->size;
}

u64 memblock_memory_size(void)
{
	return memblock.memory.total_size;
}

u64 memblock_reserved_size(void)
{
	return memblock.reserved.total_size;
}

bool memblock_is_memory(u64 addr)
{
	return memblock_search(&memblock.memory, addr) != NULL;
}

bool memblock_is_reserved(u64 addr)
{
	return memblock_search(&memblock.reserved, addr) != NULL;
}

bool memblock_is_region_memory(u64 base, u64 size)
{
	const struct memblock_region *rgn;
	u64 end;

	if (!size || base + size < base)
		return false;

	end = base + size;
	rgn = memblock_search(&memblock.memory, base);
	return rgn && end <= rgn->base + rgn->size;
}

bool memblock_is_region_reserved(u64 base, u64 size)
{
	return memblock_overlaps_region(&memblock.reserved, base, size);
}

bool memblock_overlaps_region(const struct memblock_type *type, u64 base,
			      u64 size)
{
	u64 end;

	if (!size || base + size < base)
		return false;

	end = base + size;

	for (size_t idx = 0; idx < type->cnt; ++idx) {
		const struct memblock_region *rgn = &type->regions[idx];

		if (rgn->base >= end)
			break;
		if (base < rgn->base + rgn->size)
			return true;
	}

	return false;
}

void memblock_set_current_limit(u64 limit)
{
	memblock.current_limit = limit;
}

u64 memblock_get_current_limit(void)
{
	return memblock.current_limit;
}

bool memblock_bottom_up(void)
{
	return memblock.bottom_up;
}

void memblock_set_bottom_up(bool bottom_up)
{
	memblock.bottom_up = bottom_up;
}

/**
 * @brief Decide whether an iterator should skip a region.
 *
 * NOMAP memory is not usable for ordinary direct-mapped allocations.  It is
 * skipped unless the caller explicitly asks for it with @ref MEMBLOCK_NOMAP.
 *
 * @param[in] type Type being iterated.
 * @param[in] rgn Region being considered.
 * @param[in] flags Flags explicitly requested by the caller.
 *
 * @return true if the region should be skipped.
 */
static bool memblock_should_skip_region(const struct memblock_type *type,
					const struct memblock_region *rgn,
					enum memblock_flags flags)
{
	if (type != &memblock.memory)
		return false;

	/* NOMAP memory must be requested explicitly. */
	if (!(flags & MEMBLOCK_NOMAP) && memblock_is_nomap(rgn))
		return true;

	return false;
}

void __next_mem_range(u64 *pidx, const struct memblock_type *type_a,
		      const struct memblock_type *type_b,
		      enum memblock_flags flags, u64 *pstart, u64 *pend)
{
	u32 idx_a = *pidx & 0xffffffff;
	u32 idx_b = *pidx >> 32;

	for (; idx_a < type_a->cnt; ++idx_a) {
		const struct memblock_region *a = &type_a->regions[idx_a];
		u64 astart = a->base;
		u64 aend = astart + a->size;

		if (memblock_should_skip_region(type_a, a, flags))
			continue;

		for (;;) {
			u64 bstart;
			u64 bend;

			if (!type_b) {
				bstart = 0;
				bend = UINT64_MAX;
			} else if (!idx_b) {
				bstart = 0;
				bend = type_b->cnt ? type_b->regions[0].base :
						     UINT64_MAX;
			} else if (idx_b <= type_b->cnt) {
				const struct memblock_region *prev =
					&type_b->regions[idx_b - 1];

				bstart = prev->base + prev->size;
				bend = idx_b < type_b->cnt ?
					       type_b->regions[idx_b].base :
					       UINT64_MAX;
			} else {
				break;
			}

			if (bstart >= aend)
				break;

			if (astart < bend) {
				u64 start = max(astart, bstart);
				u64 end = min(aend, bend);

				if (start < end) {
					if (pstart)
						*pstart = start;
					if (pend)
						*pend = end;

					if (aend <= bend)
						++idx_a;
					else
						++idx_b;

					*pidx = ((u64)idx_b << 32) | idx_a;
					return;
				}
			}

			if (bend >= aend)
				break;
			idx_b++;
		}
	}

	*pidx = UINT64_MAX;
}

void __next_mem_range_rev(u64 *pidx, const struct memblock_type *type_a,
			  const struct memblock_type *type_b,
			  enum memblock_flags flags, u64 *pstart, u64 *pend)
{
	u32 idx_a;
	u32 idx_b;

	if (*pidx == UINT64_MAX) {
		if (!type_a->cnt)
			goto done;
		idx_a = (u32)(type_a->cnt - 1);
		idx_b = type_b ? (u32)type_b->cnt : 0;
	} else {
		idx_a = *pidx & 0xffffffff;
		idx_b = *pidx >> 32;
	}

	for (;;) {
		const struct memblock_region *a;
		u64 astart;
		u64 aend;
		u64 bstart;
		u64 bend;

		if (idx_a >= type_a->cnt)
			goto done;

		a = &type_a->regions[idx_a];
		astart = a->base;
		aend = astart + a->size;

		if (memblock_should_skip_region(type_a, a, flags)) {
			if (!idx_a)
				goto done;
			idx_a--;
			idx_b = type_b ? (u32)type_b->cnt : 0;
			continue;
		}

		if (!type_b) {
			if (pstart)
				*pstart = astart;
			if (pend)
				*pend = aend;

			if (!idx_a)
				goto done;
			idx_a--;
			idx_b = 0;
			*pidx = ((u64)idx_b << 32) | idx_a;
			return;
		}

		bstart = !idx_b ? 0 :
				  type_b->regions[idx_b - 1].base +
					  type_b->regions[idx_b - 1].size;
		bend = idx_b < type_b->cnt ? type_b->regions[idx_b].base :
					     UINT64_MAX;

		u64 start = max(astart, bstart);
		u64 end = min(aend, bend);

		if (start < end) {
			if (pstart)
				*pstart = start;
			if (pend)
				*pend = end;

			if (!idx_b) {
				if (!idx_a)
					goto done;
				idx_a--;
				idx_b = (u32)type_b->cnt;
			} else {
				idx_b--;
			}

			*pidx = ((u64)idx_b << 32) | idx_a;
			return;
		}

		if (!idx_b) {
			if (!idx_a)
				goto done;
			idx_a--;
			idx_b = (u32)type_b->cnt;
		} else {
			idx_b--;
		}
	}

done:
	*pidx = UINT64_MAX;
}

void __next_mem_pfn_range(u64 *pidx, u64 *pstart, u64 *pend)
{
	while (*pidx < memblock.memory.cnt) {
		struct memblock_region *rgn = &memblock.memory.regions[*pidx];

		*pidx += 1;
		*pstart = phys_to_pfn(align_up(rgn->base, PAGE_SIZE));
		*pend = phys_to_pfn(
			align_down(rgn->base + rgn->size, PAGE_SIZE));
		if (*pstart < *pend)
			return;
	}
	if (*pidx >= memblock.memory.cnt)
		*pidx = UINT64_MAX;
}

/**
 * @brief Free a physical range to the buddy allocator.
 *
 * Called only after @ref memblock_free_all(), so the range is no longer tracked
 * by memblock.  The range is aligned inward to page boundaries.
 *
 * @param[in] start Start of the range.
 * @param[in] end End of the range.
 */
static void memblock_free_to_buddy(u64 start, u64 end)
{
	unsigned int order;
	size_t npgs;

	start = align_up(start, PAGE_SIZE);
	end = align_down(end, PAGE_SIZE);

	while (start < end) {
		size_t chunk_size;

		order = page_order(end - start);
		if (order > PAGE_ORDER_MAX)
			order = PAGE_ORDER_MAX;

		/*
		 * Pick the largest aligned chunk that still fits inside
		 * [start, end). page_order() may round up and would
		 * otherwise free past the end of this range.
		 */
		for (;;) {
			chunk_size = (size_t)1 << (PAGE_SHIFT + order);
			if (is_aligned(start, chunk_size) &&
			    chunk_size <= end - start)
				break;
			--order;
		}

		npgs = (1 << order);

		struct page *pg = pfn_to_page(phys_to_pfn(start));
		assert(pg);
		pg->flags = PAGE_FLAGS_NEW_PAGE;
		page_free(pg, order);

		start += (npgs << PAGE_SHIFT);
	}
}

/**
 * @brief Release all free memory to the buddy allocator.
 *
 * Reserved ranges stay reserved, memblock allocation is disabled, and
 * automatic array resizing is turned off.  This is the point where memblock
 * hands over to the buddy allocator.
 */
void memblock_free_all(void)
{
	u64 idx;
	u64 start, end;

	if (memblock_free_all_done)
		return;

	memblock_free_all_done = true;
	memblock_alloc_enabled = false;
	memblock_can_resize = false;

	assert(memblock_validate() == 0);

	for_each_free_mem_range(idx, start, end)
		memblock_free_to_buddy(start, end);
}
