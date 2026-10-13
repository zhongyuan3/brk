#include <brk/hash.h>

u32 fnv1a_32(const void *data, size_t len)
{
	u32 hash = FNV1A_32_INIT;
	const u8 *k = (const u8 *)data;
	for (size_t i = 0; i < len; i++) {
		hash ^= k[i];
		hash *= FNV1A_32_PRIME;
	}
	return hash;
}

u32 hash_combine32(u32 a, u32 b)
{
	return a ^ (b << 1);
}
