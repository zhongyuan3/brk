#ifndef BRK_SORT_H
#define BRK_SORT_H

#include <brk/base/types.h>

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *));

#endif
