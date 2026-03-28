#ifndef AOSD_VMALLOC_H
#define AOSD_VMALLOC_H

#include <aosd/mm_types.h>
#include <aosd/types.h>

enum vmap_mode {
	VMAP_MODE_EARLY,
	VMAP_MODE_INTERIM,
	VMAP_MODE_FINAL,
};

int vmap(pgde_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	 unsigned int flags, enum vmap_mode mode);
void vunmap(pgde_t *pgd, uint64_t addr, size_t size, enum vmap_mode mode);
int kvmap(uint64_t addr, size_t size, uint64_t paddr, unsigned int flags);
int kvmap_with_mode(uint64_t addr, size_t size, uint64_t paddr,
		    unsigned int flags, enum vmap_mode mode);
void kvunmap(uint64_t addr, size_t size);
int uvmap(pgde_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	  unsigned int flags);
void uvunmap(pgde_t *pgd, uint64_t addr, size_t size);

struct vm_area {
	struct list_head list;
	uint64_t addr;
	size_t size;
	struct page **pages;
	size_t nr_pages;
	unsigned int flags;
	bool is_free;
};

void vmalloc_init(void);
void *vmalloc(size_t size);
void vfree(void *ptr);
void *vmalloc_nomap(size_t size);
void vfree_nomap(void *ptr);
struct vm_area *vm_area_alloc(void);
void vm_area_free(struct vm_area *area);

#endif
