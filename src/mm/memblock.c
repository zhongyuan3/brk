#include <arch/asm/page.h>
#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/memblock.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/string.h>

#define for_each_memblock_region(set, idx, rgn)               \
	for (idx = 0, rgn = &set->regions[0]; idx < set->cnt; \
	     ++idx, rgn = &set->regions[idx])

static struct memblock_region memory[INIT_MEMBLOCK_REGIONS];
static struct memblock_region reserved[INIT_MEMBLOCK_RESERVED_REGIONS];
static struct memblock init_memblock = {
	.memory.name = "memory",
	.memory.regions = memory,
	.memory.max = INIT_MEMBLOCK_REGIONS,
	.memory.cnt = 0,
	.reserved.name = "reserved",
	.reserved.regions = reserved,
	.reserved.max = INIT_MEMBLOCK_RESERVED_REGIONS,
	.reserved.cnt = 0,
};

static void memblock_move_regions(struct memblock_region_set *set, size_t to,
				  size_t from, size_t n)
{
	memmove(&set->regions[to], &set->regions[from],
		n * sizeof(struct memblock_region));
}

static void memblock_insert_region(struct memblock_region_set *set, size_t idx,
				   u64 base, size_t size)
{
	if (idx < set->cnt)
		memblock_move_regions(set, idx + 1, idx, set->cnt - idx);
	set->regions[idx].base = base;
	set->regions[idx].size = size;
	set->cnt++;
}

static void memblock_merge_regions(struct memblock_region_set *set)
{
	if (set->cnt < 2)
		return;

	size_t idx = 0;
	while (idx < set->cnt - 1) {
		struct memblock_region *this = &set->regions[idx];
		struct memblock_region *next = &set->regions[idx + 1];

		if (this->base + this->size == next->base) {
			this->size += next->size;
			memblock_move_regions(set, idx + 1, idx + 2,
					      set->cnt - idx - 2);
			set->cnt--;
		} else {
			idx++;
		}
	}
}

static int memblock_add_region(struct memblock_region_set *set, u64 base,
			       size_t size)
{
	size_t idx;
	struct memblock_region *rgn;
	u64 old_base = base;
	u64 end = base + size;
	size_t nr_new = 0;
	bool insert = false;

	if (size == 0)
		return 0;

	if (set->cnt == 0) {
		rgn = &set->regions[0];
		rgn->base = base;
		rgn->size = size;
		set->cnt++;
		return 0;
	}

	if (set->cnt * 2 + 1 <= set->max)
		insert = true;

repeat:
	base = old_base;
	nr_new = 0;

	for_each_memblock_region(set, idx, rgn) {
		u64 rbase = rgn->base;
		u64 rend = rbase + rgn->size;

		if (end <= rbase)
			break;

		if (base >= rend)
			continue;

		if (base < rbase) {
			nr_new++;
			if (insert)
				memblock_insert_region(set, idx++, base,
						       rbase - base);
		}

		base = min(end, rend);
	}

	if (end > base) {
		nr_new++;
		if (insert)
			memblock_insert_region(set, idx, base, end - base);
	}

	if (!insert) {
		if (nr_new > set->max - set->cnt)
			panic("%s(): region count overflow\n", __func__);
		insert = true;
		goto repeat;
	} else {
		memblock_merge_regions(set);
		return 0;
	}
}

int memblock_add(u64 base, size_t size)
{
	return memblock_add_region(&init_memblock.memory, base, size);
}

int memblock_reserve(u64 base, size_t size)
{
	return memblock_add_region(&init_memblock.reserved, base, size);
}

u64 memblock_alloc(size_t size, u64 min_addr, size_t align)
{
	u64 start, end;
	u64 idx;

	ASSERT(is_power_of_two(align));

	for_each_mem_range(idx, start, end) {
		if (start < min_addr)
			continue;

		u64 aligned_start = round_up(max(start, min_addr), align);
		if (aligned_start + size > end)
			continue;

		memblock_reserve(aligned_start, size);
		return aligned_start;
	}

	return 0;
}

static int memblock_remove_region(struct memblock_region_set *set,
				  size_t rgn_idx, u64 base, size_t size)
{
	u64 end = base + size;
	struct memblock_region *rgn = &set->regions[rgn_idx];
	u64 rbase = rgn->base;
	u64 rend = rbase + rgn->size;

	ASSERT(base >= rbase && end <= rend);

	if (base == rbase) {
		if (end == rend) {
			memblock_move_regions(set, rgn_idx, rgn_idx + 1,
					      set->cnt - rgn_idx - 1);
			set->cnt--;
		} else {
			rgn->base = end;
			rgn->size = rend - end;
		}
		return 0;
	}

	/* Now base is greater than rbase */
	if (end == rend) {
		rgn->size = base - rbase;
		return 0;
	}

	/* rbase < base < end < rend */
	if (set->cnt > set->max - 1)
		panic("%s(): region count overflow\n", __func__);

	/* Split the region */
	memblock_insert_region(set, rgn_idx + 1, end, rend - end);
	rgn->size = base - rbase;
	return 0;
}

static bool memblock_find_region(struct memblock_region_set *set, u64 base,
				 size_t size, size_t *pidx,
				 struct memblock_region *prgn)
{
	size_t idx;
	struct memblock_region *rgn;
	u64 end = base + size;

	for_each_memblock_region(set, idx, rgn) {
		if (base >= rgn->base && end <= rgn->base + rgn->size) {
			*pidx = idx;
			prgn->base = base;
			prgn->size = size;
			return true;
		}
	}

	return false;
}

void memblock_free(u64 base, size_t size)
{
	size_t idx;
	struct memblock_region rgn;

	if (!memblock_find_region(&init_memblock.reserved, base, size, &idx,
				  &rgn))
		return;

	memblock_remove_region(&init_memblock.reserved, idx, rgn.base,
			       rgn.size);
}

u64 memblock_start_of_dram(void)
{
	if (init_memblock.memory.cnt == 0)
		return 0;

	return init_memblock.memory.regions[0].base;
}

void memblock_dump_all(void)
{
	printk("MEMBLOCK configuration:\n");
	memblock_dump(&init_memblock.memory);
	memblock_dump(&init_memblock.reserved);
}

void __next_mem_range(u64 *pidx, u64 *pstart, u64 *pend)
{
	u32 idx_mem = (*pidx >> 32) & 0xffffffffUL;
	u32 idx_res = *pidx & 0xffffffffUL;
	struct memblock_region_set *mem = &init_memblock.memory;
	struct memblock_region_set *res = &init_memblock.reserved;

	for (; idx_mem < mem->cnt; ++idx_mem) {
		struct memblock_region *m;
		u64 mstart;
		u64 mend;

		m = &mem->regions[idx_mem];
		mstart = m->base;
		mend = mstart + m->size;

		if (!res) {
			if (pstart)
				*pstart = mstart;
			if (pend)
				*pend = mend;
			idx_mem += 1;
			*pidx = ((u64)idx_mem << 32) | idx_res;
			return;
		}

		for (; idx_res < res->cnt + 1; ++idx_res) {
			struct memblock_region *r;
			u64 rstart;
			u64 rend;

			r = &res->regions[idx_res];
			rstart = idx_res == 0 ? 0 : r[-1].base + r[-1].size;
			rend = idx_res < res->cnt ? r->base : UINT64_MAX;

			if (rstart >= mend)
				break;

			if (mstart < rend) {
				if (pstart)
					*pstart = max(mstart, rstart);
				if (pend)
					*pend = min(mend, rend);

				if (mend <= rend)
					++idx_mem;
				else
					++idx_res;

				*pidx = ((u64)idx_mem << 32) | idx_res;
				return;
			}
		}
	}

	*pidx = UINT64_MAX;
}

void __next_mem_pfn_range(u32 *pidx, u64 *pstart, u64 *pend)
{
	struct memblock_region_set *mem = &init_memblock.memory;
	while (*pidx < mem->cnt) {
		struct memblock_region *rgn = &mem->regions[*pidx];
		*pidx += 1;
		*pstart = phys_to_pfn(round_up(rgn->base, PAGE_SIZE));
		*pend = phys_to_pfn(
			round_down(rgn->base + rgn->size, PAGE_SIZE));
		if (*pstart < *pend)
			return;
	}
	if (*pidx >= mem->cnt)
		*pidx = UINT32_MAX;
}

static void memblock_free_range(u64 start, u64 end)
{
	unsigned int order;
	size_t npgs;

	start = round_up(start, PAGE_SIZE);
	end = round_down(end, PAGE_SIZE);

	klog_debug("memblock: free range: %#lx-%#lx\n", start, end);
	memblock_reserve(start, end - start);

	while (start < end) {
		order = page_order(end - start);
		if (order > PAGE_ORDER_MAX)
			order = PAGE_ORDER_MAX;
		while (!is_aligned(start, (size_t)1 << (PAGE_SHIFT + order)))
			--order;

		npgs = (1 << order);

		struct page *pg = pfn_to_page(phys_to_pfn(start));
		pg->flags = PAGE_FLAGS_NEW_PAGE;
		ASSERT(pg);
		page_free(pg, order);

		start += (npgs << PAGE_SHIFT);
	}
}

void memblock_free_all(void)
{
	u64 idx;
	u64 start, end;

	for_each_mem_range(idx, start, end) {
		memblock_free_range(start, end);
	}

	klog_debug("memblock: %s() done\n", __func__);
}

void memblock_dump(struct memblock_region_set *set)
{
	size_t idx;
	struct memblock_region *rgn;

	printk(" %s.cnt=%zu\n", set->name, set->cnt);
	for_each_memblock_region(set, idx, rgn) {
		u64 start = rgn->base;
		size_t size = rgn->size;
		u64 end = start + size;
		printk(" %s[%zu]\t[%p-%p], %zu bytes\n", set->name, idx,
		       (void *)(uintptr_t)start, (void *)(uintptr_t)end, size);
	}
}
