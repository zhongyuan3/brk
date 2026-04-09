#ifndef STDLIB_H
#define STDLIB_H

#include <brk/slab.h>

#define malloc(size) kmalloc(size)
#define calloc(nmemb, size) kcalloc(nmemb, size)
#define free(ptr) kfree(ptr)

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *));

#endif
