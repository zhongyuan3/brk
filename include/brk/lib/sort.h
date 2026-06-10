#ifndef BRK_SORT_H
#define BRK_SORT_H

#include <brk/lib/types.h>

void qsort(void *base, usize_t nmemb, usize_t size,
	   int (*compar)(const void *, const void *));

#endif
