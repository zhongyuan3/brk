#include <brk/align.h>
#include <brk/assert.h>
#include <brk/macros.h>
#include <brk/memblock.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/string.h>

static struct memblock_region memory[INIT_MEMBLOCK_REGIONS];
static struct memblock_region reserved[INIT_MEMBLOCK_RESERVED_REGIONS];
static struct memblock init_memblock;

static void memblock_move_regions(struct memblock_type *type, size_t to,
				  size_t from, size_t n)
{
	memmove(&type->regions[to], &type->regions[from],
		n * sizeof(struct memblock_region));
}

static void memblock_insert_region(struct memblock_type *type, size_t idx,
				   uint64_t base, size_t size)
{
	if (idx < type->cnt)
		memblock_move_regions(type, idx + 1, idx, type->cnt - idx);
	type->regions[idx].base = base;
	type->regions[idx].size = size;
	type->cnt++;
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

static int memblock_add_region(struct memblock_type *type, uint64_t base,
			       size_t size)
{
	size_t idx;
	struct memblock_region *rgn;
	uint64_t old_base = base;
	uint64_t end = base + size;
	size_t nr_new = 0;
	bool insert = false;

	if (size == 0)
		return 0;

	if (type->cnt == 0) {
		rgn = &type->regions[0];
		rgn->base = base;
		rgn->size = size;
		type->cnt++;
		return 0;
	}

	if (type->cnt * 2 + 1 <= type->max)
		insert = true;

repeat:
	base = old_base;
	nr_new = 0;

	for_each_memblock_type(type, idx, rgn) {
		uint64_t rbase = rgn->base;
		uint64_t rend = rbase + rgn->size;

		if (end <= rbase)
			break;

		if (base >= rend)
			continue;

		if (base < rbase) {
			nr_new++;
			if (insert)
				memblock_insert_region(type, idx++, base,
						       rbase - base);
		}

		base = min(end, rend);
	}

	if (end > base) {
		nr_new++;
		if (insert)
			memblock_insert_region(type, idx, base, end - base);
	}

	if (!insert) {
		if (nr_new > type->max - type->cnt)
			panic("%s(): region count overflow\n", __func__);
		insert = true;
		goto repeat;
	} else {
		memblock_merge_regions(type);
		return 0;
	}
}

void memblock_init(void)
{
	init_memblock.memory.name = "memory";
	init_memblock.memory.regions = memory;
	init_memblock.memory.max = INIT_MEMBLOCK_REGIONS;
	init_memblock.memory.cnt = 0;
	init_memblock.reserved.name = "reserved";
	init_memblock.reserved.regions = reserved;
	init_memblock.reserved.max = INIT_MEMBLOCK_RESERVED_REGIONS;
	init_memblock.reserved.cnt = 0;
}

int memblock_add(uint64_t base, size_t size)
{
	return memblock_add_region(&init_memblock.memory, base, size);
}

int memblock_reserve(uint64_t base, size_t size)
{
	return memblock_add_region(&init_memblock.reserved, base, size);
}

uint64_t memblock_alloc(size_t size, uint64_t min_addr, size_t align)
{
	uint64_t start, end;
	uint64_t idx;

	assert(is_pow2(align));

	for_each_mem_range(idx, start, end) {
		if (start < min_addr)
			continue;

		uint64_t aligned_start = align_up(max(start, min_addr), align);
		if (aligned_start + size > end)
			continue;

		memblock_reserve(aligned_start, size);
		return aligned_start;
	}

	return 0;
}

static int memblock_remove_region(struct memblock_type *type, size_t rgn_idx,
				  uint64_t base, size_t size)
{
	uint64_t end = base + size;
	struct memblock_region *rgn = &type->regions[rgn_idx];
	uint64_t rbase = rgn->base;
	uint64_t rend = rbase + rgn->size;

	assert(base >= rbase && end <= rend);

	if (base == rbase) {
		if (end == rend) {
			memblock_move_regions(type, rgn_idx, rgn_idx + 1,
					      type->cnt - rgn_idx - 1);
			type->cnt--;
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
	if (type->cnt > type->max - 1)
		panic("%s(): region count overflow\n", __func__);

	/* Split the region */
	memblock_insert_region(type, rgn_idx + 1, end, rend - end);
	rgn->size = base - rbase;
	return 0;
}

static bool memblock_find_region(struct memblock_type *type, uint64_t base,
				 size_t size, size_t *pidx,
				 struct memblock_region *prgn)
{
	size_t idx;
	struct memblock_region *rgn;
	uint64_t end = base + size;

	for_each_memblock_type(type, idx, rgn) {
		if (base >= rgn->base && end <= rgn->base + rgn->size) {
			*pidx = idx;
			prgn->base = base;
			prgn->size = size;
			return true;
		}
	}

	return false;
}

void memblock_free(uint64_t base, size_t size)
{
	size_t idx;
	struct memblock_region rgn;

	if (!memblock_find_region(&init_memblock.reserved, base, size, &idx,
				  &rgn))
		return;

	memblock_remove_region(&init_memblock.reserved, idx, rgn.base,
			       rgn.size);
}

uint64_t memblock_get_ram_base(void)
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

void __next_mem_range(uint64_t *pidx, uint64_t *pstart, uint64_t *pend)
{
	uint32_t idx_mem = (*pidx >> 32) & 0xffffffffUL;
	uint32_t idx_res = *pidx & 0xffffffffUL;
	struct memblock_type *mem = &init_memblock.memory;
	struct memblock_type *res = &init_memblock.reserved;

	for (; idx_mem < mem->cnt; ++idx_mem) {
		struct memblock_region *m;
		uint64_t mstart;
		uint64_t mend;

		m = &mem->regions[idx_mem];
		mstart = m->base;
		mend = mstart + m->size;

		if (!res) {
			if (pstart)
				*pstart = mstart;
			if (pend)
				*pend = mend;
			idx_mem += 1;
			*pidx = ((uint64_t)idx_mem << 32) | idx_res;
			return;
		}

		for (; idx_res < res->cnt + 1; ++idx_res) {
			struct memblock_region *r;
			uint64_t rstart;
			uint64_t rend;

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

				*pidx = ((uint64_t)idx_mem << 32) | idx_res;
				return;
			}
		}
	}

	*pidx = UINT64_MAX;
}

void __next_mem_pfn_range(uint32_t *pidx, uint64_t *pstart, uint64_t *pend)
{
	struct memblock_type *mem = &init_memblock.memory;
	if (*pidx >= mem->cnt) {
		*pidx = UINT32_MAX;
		return;
	}
	struct memblock_region *rgn = &mem->regions[*pidx];
	*pstart = phys_to_pfn(align_up(rgn->base, PAGE_SIZE));
	*pend = phys_to_pfn(align_down(rgn->base + rgn->size, PAGE_SIZE));
	*pidx += 1;
}

static void memblock_free_range(uint64_t start, uint64_t end)
{
	unsigned int order;
	size_t npgs;

	while (start < end) {
		order = page_order(end - start);
		if (order > PAGE_ORDER_MAX)
			order = PAGE_ORDER_MAX;
		while (!is_aligned(start, (size_t)1 << (PAGE_SHIFT + order)))
			--order;

		npgs = (1 << order);

		struct page *pg = pfn_to_page(phys_to_pfn(start));
		pg->flags = PAGE_FLAGS_NEW_PAGE;
		assert(pg);
		page_free(pg, order);

		start += (npgs << PAGE_SHIFT);
	}
}

void memblock_free_all(void)
{
	uint64_t idx;
	uint64_t start, end;

	uint64_t bases[INIT_MEMBLOCK_REGIONS];
	size_t sizes[INIT_MEMBLOCK_REGIONS];
	size_t cnt = 0;

	for_each_mem_range(idx, start, end) {
		memblock_free_range(start, end);
		bases[cnt] = start;
		sizes[cnt++] = end - start;
	}

	for (size_t i = 0; i < cnt; ++i)
		memblock_reserve(bases[i], sizes[i]);
}

void memblock_dump(struct memblock_type *type)
{
	size_t idx;
	struct memblock_region *rgn;

	printk(" %s.cnt=%zu\n", type->name, type->cnt);
	for_each_memblock_type(type, idx, rgn) {
		uint64_t start = rgn->base;
		size_t size = rgn->size;
		uint64_t end = start + size;
		printk(" %s[%zu]\t[%p-%p], %zx bytes\n", type->name, idx,
		       (void *)start, (void *)end, size);
	}
}
