#ifndef STDLIB_H
#define STDLIB_H

#include <aosd/slab.h>

static inline void *malloc(size_t size)
{
	return kmalloc(size);
}

static inline void *calloc(size_t nmemb, size_t size)
{
	return kcalloc(nmemb, size);
}

static inline void free(void *ptr)
{
	kfree(ptr);
}

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *));

#endif
