#ifndef STDLIB_H
#define STDLIB_H

#include <brk/types.h>

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *));

#endif
