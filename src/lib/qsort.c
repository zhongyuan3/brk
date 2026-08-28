#include <brk/lib/sort.h>
#include <brk/lib/string.h>

static void heapify(void *base, size_t nmemb, size_t size, size_t i,
		    int (*compar)(const void *, const void *))
{
	size_t largest = i;
	size_t l = 2 * i + 1;
	size_t r = 2 * i + 2;

	if (l < nmemb &&
	    compar((char *)base + l * size, (char *)base + largest * size) > 0)
		largest = l;

	if (r < nmemb &&
	    compar((char *)base + r * size, (char *)base + largest * size) > 0)
		largest = r;

	if (largest != i) {
		memswap((char *)base + i * size, (char *)base + largest * size,
			size);
		heapify(base, nmemb, size, largest, compar);
	}
}

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *))
{
	if (nmemb <= 1 || base == NULL || compar == NULL)
		return;

	long i;

	for (i = (long)(nmemb / 2) - 1; i >= 0; --i)
		heapify(base, nmemb, size, (size_t)i, compar);

	for (i = (long)(nmemb - 1); i > 0; --i) {
		memswap(base, (char *)base + (size_t)i * size, size);
		heapify(base, (size_t)i, size, 0, compar);
	}
}
