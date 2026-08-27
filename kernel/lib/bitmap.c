#include <brk/base/bits.h>
#include <brk/base/kernel.h>
#include <brk/lib/bitmap.h>
#include <brk/lib/string.h>

static unsigned long bitmap_last_word_mask(size_t nbits)
{
	size_t rem = nbits % BITS_PER_LONG;

	if (rem == 0)
		return ~0UL;

	return BIT_MASK(rem);
}

void bitmap_zero(unsigned long *bitmap, size_t nbits)
{
	size_t nlongs = BITS_TO_LONGS(nbits);

	memset(bitmap, 0, nlongs * sizeof(*bitmap));
}

void bitmap_fill(unsigned long *bitmap, size_t nbits)
{
	size_t nlongs = BITS_TO_LONGS(nbits);

	memset(bitmap, 0xFF, nlongs * sizeof(*bitmap));
	if (nlongs != 0)
		bitmap[nlongs - 1] &= bitmap_last_word_mask(nbits);
}

void bitmap_set_bit(unsigned long *bitmap, size_t bit)
{
	size_t idx = bit / BITS_PER_LONG;
	size_t ofs = bit % BITS_PER_LONG;

	bitmap[idx] |= BIT(ofs);
}

void bitmap_clear_bit(unsigned long *bitmap, size_t bit)
{
	size_t idx = bit / BITS_PER_LONG;
	size_t ofs = bit % BITS_PER_LONG;

	bitmap[idx] &= ~BIT(ofs);
}

bool bitmap_test_bit(const unsigned long *bitmap, size_t bit)
{
	size_t idx = bit / BITS_PER_LONG;
	size_t ofs = bit % BITS_PER_LONG;

	return (bitmap[idx] & BIT(ofs)) != 0;
}

size_t bitmap_find_first_zero_bit(const unsigned long *bitmap, size_t nbits)
{
	return bitmap_find_next_zero_bit(bitmap, nbits, 0);
}

size_t bitmap_find_next_zero_bit(const unsigned long *bitmap, size_t nbits,
				 size_t start)
{
	size_t bit;

	if (start >= nbits)
		return nbits;

	for (bit = start; bit < nbits; bit++) {
		if (!bitmap_test_bit(bitmap, bit)) {
			return bit;
		}
	}

	return nbits;
}

bool bitmap_alloc_bit(unsigned long *bitmap, size_t nbits, size_t *out_bit)
{
	size_t bit = bitmap_find_first_zero_bit(bitmap, nbits);

	if (bit >= nbits)
		return false;

	bitmap_set_bit(bitmap, bit);
	if (out_bit != NULL)
		*out_bit = bit;

	return true;
}

bool bitmap_alloc_bit_from(unsigned long *bitmap, size_t nbits, size_t hint,
			   size_t *out_bit)
{
	size_t bit;

	if (nbits == 0)
		return false;

	hint = min(hint, nbits);

	bit = bitmap_find_next_zero_bit(bitmap, nbits, hint);
	if (bit >= nbits) {
		bit = bitmap_find_next_zero_bit(bitmap, nbits, 0);
		if (bit >= hint)
			return false;
	}
	bitmap_set_bit(bitmap, bit);

	if (out_bit != NULL)
		*out_bit = bit;

	return true;
}

void bitmap_free_bit(unsigned long *bitmap, size_t nbits, size_t bit)
{
	if (bit < nbits)
		bitmap_clear_bit(bitmap, bit);
}

bool bitmap_alloc_region(unsigned long *bitmap, size_t nbits, size_t len,
			 size_t align, size_t *out_start)
{
	size_t cand;

	if (len == 0 || len > nbits)
		return false;

	if (align == 0)
		align = 1;

	for (cand = bitmap_find_first_zero_bit(bitmap, nbits);
	     cand + len <= nbits;
	     cand = bitmap_find_next_zero_bit(bitmap, nbits, cand + 1)) {
		size_t i;

		if (align > 1 && (cand % align) != 0)
			continue;

		for (i = 0; i < len; i++) {
			if (bitmap_test_bit(bitmap, cand + i))
				break;
		}

		if (i != len)
			continue;

		for (i = 0; i < len; i++)
			bitmap_set_bit(bitmap, cand + i);

		if (out_start != NULL)
			*out_start = cand;

		return true;
	}
	return false;
}

void bitmap_free_region(unsigned long *bitmap, size_t nbits, size_t start,
			size_t len)
{
	size_t i;

	for (i = 0; i < len && start + i < nbits; i++)
		bitmap_clear_bit(bitmap, start + i);
}
