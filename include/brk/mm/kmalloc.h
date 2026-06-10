#ifndef BRK_KALLOC_H
#define BRK_KALLOC_H

#include <brk/lib/types.h>

void kmalloc_init(void);
void *kmalloc(usize_t size);
void *kcalloc(usize_t nmemb, usize_t size);
void *kzalloc(usize_t size);
void kfree(void *ptr);

#endif
