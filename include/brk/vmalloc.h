#ifndef BRK_VMALLOC_H
#define BRK_VMALLOC_H

#include <brk/mm_types.h>
#include <brk/types.h>

enum vmap_mode {
	VMAP_MODE_EARLY,
	VMAP_MODE_INTERIM,
	VMAP_MODE_FINAL,
};

int vmap(pgde_t *pgd, u64 addr, usize_t size, u64 paddr, unsigned int flags,
	 enum vmap_mode mode);
void vunmap(pgde_t *pgd, u64 addr, usize_t size, enum vmap_mode mode);
int kvmap(u64 addr, usize_t size, u64 paddr, unsigned int flags);
int kvmap_with_mode(u64 addr, usize_t size, u64 paddr, unsigned int flags,
		    enum vmap_mode mode);
void kvunmap(u64 addr, usize_t size);
int uvmap(pgde_t *pgd, u64 addr, usize_t size, u64 paddr, unsigned int flags);
void uvunmap(pgde_t *pgd, u64 addr, usize_t size);

struct vm_area {
	struct list_head list;
	u64 addr;
	usize_t size;
	struct page **pages;
	usize_t nr_pages;
	unsigned int flags;
	bool is_free;
};

void vmalloc_init(void);
void *vmalloc(usize_t size);
void vfree(void *ptr);
void *vmalloc_nomap(usize_t size);
void vfree_nomap(void *ptr);
struct vm_area *vm_area_alloc(void);
void vm_area_free(struct vm_area *area);

#endif
