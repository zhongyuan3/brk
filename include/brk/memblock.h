#ifndef BRK_MEMBLOCK_H
#define BRK_MEMBLOCK_H

#include <brk/types.h>

#define INIT_MEMBLOCK_REGIONS 32
#define INIT_MEMBLOCK_RESERVED_REGIONS 32

struct memblock_region {
	u64 base;
	usize_t size;
};

struct memblock_type {
	char const *name;
	struct memblock_region *regions;
	usize_t max;
	usize_t cnt;
};

struct memblock {
	struct memblock_type memory;
	struct memblock_type reserved;
};

void memblock_init(void);
int memblock_add(u64 base, usize_t size);
int memblock_reserve(u64 base, usize_t size);
u64 memblock_alloc(usize_t size, u64 min_addr, usize_t align);
void memblock_free(u64 base, usize_t size);
u64 memblock_get_ram_base(void);
void memblock_free_all(void);
void memblock_dump(struct memblock_type *type);
void memblock_dump_all(void);

void __next_mem_range(u64 *pidx, u64 *pstart, u64 *pend);
void __next_mem_pfn_range(u32 *pidx, u64 *pstart, u64 *pend);

#define for_each_mem_range(idx, start, end)                                    \
	for (idx = 0, __next_mem_range(&idx, &start, &end); idx != UINT64_MAX; \
	     __next_mem_range(&idx, &start, &end))

#define for_each_mem_pfn_range(idx, start, end)                 \
	for (idx = 0, __next_mem_pfn_range(&idx, &start, &end); \
	     idx != UINT32_MAX; __next_mem_pfn_range(&idx, &start, &end))

#define for_each_memblock_type(type, idx, rgn)                  \
	for (idx = 0, rgn = &type->regions[0]; idx < type->cnt; \
	     ++idx, rgn = &type->regions[idx])

#endif
