#ifndef BRK_MM_H
#define BRK_MM_H

#include <arch/pgtable_types.h>
#include <brk/mm_types.h>

void switch_pgtable(pgd_t *pgd);

void user_access_enable(void);
void user_access_disable(void);

void uvm_space_cache_init(void);
struct uvm_space *uvm_space_create(void);
struct uvm_space *uvm_space_get(struct uvm_space *mm);
void uvm_space_put(struct uvm_space *mm);
int uvm_space_copy(struct uvm_space *dst, struct uvm_space *src);

struct uvm_region *uvm_region_alloc(void);
void uvm_region_free(struct uvm_region *region);

struct map_pages_context {
	pgd_t *root;
	void *addr;
	unsigned long paddr;
	unsigned long size;
	unsigned int flags;
	unsigned long (*alloc_frame)(void);
	void *(*phys_to_virt)(unsigned long);
};

int map_pages(const struct map_pages_context *ctx);

#endif
