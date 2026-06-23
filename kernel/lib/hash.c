#include <brk/hash.h>

uint32_t fnv1a_32(const void *data, size_t len)
{
	uint32_t hash = FNV1A_32_INIT;
	const uint8_t *k = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		hash ^= k[i];
		hash *= FNV1A_32_PRIME;
	}
	return hash;
}

uint32_t hash_combine32(uint32_t a, uint32_t b)
{
	return a ^ (b << 1);
}
