#ifndef BRK_ALIGN_H
#define BRK_ALIGN_H

#define align_up(x, align)                            \
	({                                            \
		__auto_type __x = (x);                \
		__auto_type __align = (align);        \
		(__x + __align - 1) & ~(__align - 1); \
	})

#define align_down(x, align)                   \
	({                                     \
		__auto_type __x = (x);         \
		__auto_type __align = (align); \
		(__x & ~(__align - 1));        \
	})

#define is_aligned(x, align)                   \
	({                                     \
		__auto_type __x = (x);         \
		__auto_type __align = (align); \
		(__x & (__align - 1)) == 0;    \
	})

#endif
