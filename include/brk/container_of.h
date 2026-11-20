#ifndef BRK_CONTAINER_OF_H
#define BRK_CONTAINER_OF_H

#define container_of(ptr, type, member)                              \
	({                                                           \
		typeof((((type *)0)->member)) const *__mptr = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member));   \
	})

#endif
