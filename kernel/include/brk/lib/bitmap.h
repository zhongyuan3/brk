#ifndef BRK_BITMAP_H
#define BRK_BITMAP_H

#include <brk/lib/types.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(nbits) (((nbits) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define BITMAP_DECLARE(name, nbits) unsigned long name[BITS_TO_LONGS(nbits)]

void bitmap_zero(unsigned long *bitmap, usize_t nbits);
void bitmap_fill(unsigned long *bitmap, usize_t nbits);

void bitmap_set_bit(unsigned long *bitmap, usize_t bit);
void bitmap_clear_bit(unsigned long *bitmap, usize_t bit);
bool bitmap_test_bit(const unsigned long *bitmap, usize_t bit);

usize_t bitmap_find_first_zero_bit(const unsigned long *bitmap, usize_t nbits);
usize_t bitmap_find_next_zero_bit(const unsigned long *bitmap, usize_t nbits,
				  usize_t start);

/*
 * Allocation helpers: "free" is represented by a cleared bit (0), "in use" by a set bit (1).
 * bitmap_alloc_* sets bits; bitmap_free_* clears bits.
 */

bool bitmap_alloc_bit(unsigned long *bitmap, usize_t nbits, usize_t *out_bit);
bool bitmap_alloc_bit_from(unsigned long *bitmap, usize_t nbits, usize_t hint,
			   usize_t *out_bit);
void bitmap_free_bit(unsigned long *bitmap, usize_t nbits, usize_t bit);

/*
 * Allocate or free len consecutive bits. align is 1 for no alignment, otherwise the start
 * index must be a multiple of align (align should be a power of two).
 */
bool bitmap_alloc_region(unsigned long *bitmap, usize_t nbits, usize_t len,
			 usize_t align, usize_t *out_start);
void bitmap_free_region(unsigned long *bitmap, usize_t nbits, usize_t start,
			usize_t len);

#endif
