#ifndef BRK_MM_H
#define BRK_MM_H

#include <arch/mm.h>
#include <arch/pgalloc.h>
#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/printk.h>
#include <brk/types.h>
#include <brk/vmalloc.h>

void uvm_space_cache_init(void);
struct uvm_space *uvm_space_create(void);
struct uvm_space *uvm_space_get(struct uvm_space *mm);
void uvm_space_put(struct uvm_space *mm);
int uvm_space_copy(struct uvm_space *dst, struct uvm_space *src);

struct uvm_region *uvm_region_alloc(void);
void uvm_region_free(struct uvm_region *region);

#endif
