#ifndef BRK_VMALLOC_H
#define BRK_VMALLOC_H

#include <brk/types.h>

void vmalloc_init(void);
void *vmalloc(size_t size);
void vfree(void *ptr);
void *vmalloc_nomap(size_t size);
void vfree_nomap(void *ptr);

#endif
