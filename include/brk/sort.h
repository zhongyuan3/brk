#ifndef _BRK_SORT_H
#define _BRK_SORT_H

#include <stddef.h>

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *));

#endif
