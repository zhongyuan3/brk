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
				  u64 base, u64 size)
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

		if (this->base + this->size == next->base) {
			this->size += next->size;
			memblock_move_regions(type, idx + 1, idx + 2,
					      type->cnt - idx - 2);
			type->cnt--;
		} else {
			idx++;
		}
	}
}

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

			/* Regions must be sorted, non-overlapping and merged. */
			if (rgn->base + rgn->size >= next->base)
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

static int memblock_add_region(struct memblock_type *type, u64 base, u64 size)
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
				err = memblock_insert_region(type, idx++, base,
							     rbase - base);
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
						     end - base);
			if (err)
				return err;
		}
	}

	if (!insert) {
		if (nr_new > type->max - type->cnt)
			return -ENOMEM;
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
	return memblock_add_region(&memblock.memory, base, size);
}

int memblock_reserve(u64 base, u64 size)
{
	return memblock_add_region(&memblock.reserved, base, size);
}

static int memblock_remove_range(struct memblock_type *type, u64 base, u64 size)
{
	size_t idx = 0;
	u64 end;
	int err;

	if (!size)
		return 0;
	if (base + size < base)
		return -EINVAL;

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
		err = memblock_insert_region(type, idx + 1, end, rend - end);
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

void __next_mem_range(u64 *pidx, const struct memblock_type *type_a,
		      const struct memblock_type *type_b, u64 *pstart,
		      u64 *pend)
{
	u32 idx_a = *pidx & 0xffffffff;
	u32 idx_b = *pidx >> 32;

	for (; idx_a < type_a->cnt; ++idx_a) {
		const struct memblock_region *a = &type_a->regions[idx_a];
		u64 astart = a->base;
		u64 aend = astart + a->size;

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
			  const struct memblock_type *type_b, u64 *pstart,
			  u64 *pend)
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

void memblock_free_all(void)
{
	u64 idx;
	u64 start, end;

	if (memblock_free_all_done)
		return;

	memblock_free_all_done = true;
	memblock_alloc_enabled = false;

	assert(memblock_validate() == 0);

	for_each_free_mem_range(idx, start, end)
		memblock_free_to_buddy(start, end);
}
