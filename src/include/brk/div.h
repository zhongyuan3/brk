#ifndef BRK_DIV_H
#define BRK_DIV_H

#define div_ceil(num, div)                   \
	({                                   \
		__auto_type __num = (num);   \
		__auto_type __div = (div);   \
		(__num + __div - 1) / __div; \
	})

#endif
