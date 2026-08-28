#ifndef BRK_KALLOC_H
#define BRK_KALLOC_H

#include <brk/base/types.h>

void kmalloc_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t nmemb, size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

#endif
