#ifndef AOSD_VMALLOC_H
#define AOSD_VMALLOC_H

#include <aosd/mm_types.h>
#include <aosd/types.h>

struct vmap_ops {
	uint64_t (*alloc_pgd)(void);
	pgde_t *(*get_pgd_virt)(uint64_t pgd_phys);
	uint64_t (*alloc_pmd)(void);
	pmde_t *(*get_pmd_virt)(uint64_t pmd_phys);
	uint64_t (*alloc_pt)(void);
	pte_t *(*get_pt_virt)(uint64_t pt_phys);
};

int vmap(pgde_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	 unsigned int flags, struct vmap_ops *ops);
void vunmap(pgde_t *pgd, uint64_t addr, size_t size, struct vmap_ops *ops);
int kvmap(uint64_t addr, size_t size, uint64_t paddr, unsigned int flags);
void kvunmap(uint64_t addr, size_t size);
int uvmap(pgde_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	  unsigned int flags);
void uvunmap(pgde_t *pgd, uint64_t addr, size_t size);

struct vmem_area {
	struct list_head list;
	uint64_t addr;
	size_t size;
	struct page **pages;
	size_t nr_pages;
	bool is_free;
};

void vmalloc_init(void);
void *vmalloc(size_t size);
void vfree(void *ptr);
void *vmalloc_nomap(size_t size);
void vfree_nomap(void *ptr);

#endif
