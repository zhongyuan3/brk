#ifndef BRK_VMALLOC_H
#define BRK_VMALLOC_H

#include <arch/pgtable_types.h>
#include <brk/base/types.h>
#include <brk/mm/mm_types.h>

enum vmap_mode {
	VMAP_MODE_EARLY,
	VMAP_MODE_INTERIM,
	VMAP_MODE_FINAL,
};

int vmap(pgd_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	 unsigned int flags, enum vmap_mode mode);
void vunmap(pgd_t *pgd, uint64_t addr, size_t size, enum vmap_mode mode);
int kvmap(uint64_t addr, size_t size, uint64_t paddr, unsigned int flags);
int kvmap_with_mode(uint64_t addr, size_t size, uint64_t paddr,
		    unsigned int flags, enum vmap_mode mode);
void kvunmap(uint64_t addr, size_t size);
int uvmap(pgd_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	  unsigned int flags);
void uvunmap(pgd_t *pgd, uint64_t addr, size_t size);

void vmalloc_init(void);
void *vmalloc(size_t size);
void vfree(void *ptr);
void *vmalloc_nomap(size_t size);
void vfree_nomap(void *ptr);

#endif
