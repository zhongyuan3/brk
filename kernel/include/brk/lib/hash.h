#ifndef BRK_HASH_H
#define BRK_HASH_H

#include <brk/lib/types.h>

#define FNV1A_32_INIT 0x811C9DC5
#define FNV1A_32_PRIME 0x01000193

u32 fnv1a_32(const void *data, usize_t len);
u32 hash_combine32(u32 a, u32 b);

#endif
