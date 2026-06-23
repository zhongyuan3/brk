#ifndef BRK_VMALLOC_H
#define BRK_VMALLOC_H

#include <arch/pgtable_types.h>
#include <brk/mm_types.h>
#include <brk/types.h>

enum vmap_mode {
	VMAP_MODE_EARLY,
	VMAP_MODE_INTERIM,
	VMAP_MODE_FINAL,
};

int vmap(pgde_t *pgd, u64 addr, size_t size, u64 paddr, unsigned int flags,
	 enum vmap_mode mode);
void vunmap(pgde_t *pgd, u64 addr, size_t size, enum vmap_mode mode);
int kvmap(u64 addr, size_t size, u64 paddr, unsigned int flags);
int kvmap_with_mode(u64 addr, size_t size, u64 paddr, unsigned int flags,
		    enum vmap_mode mode);
void kvunmap(u64 addr, size_t size);
int uvmap(pgde_t *pgd, u64 addr, size_t size, u64 paddr, unsigned int flags);
void uvunmap(pgde_t *pgd, u64 addr, size_t size);

void vmalloc_init(void);
void *vmalloc(size_t size);
void vfree(void *ptr);
void *vmalloc_nomap(size_t size);
void vfree_nomap(void *ptr);

#endif
