#ifndef BRK_HASH_H
#define BRK_HASH_H

#include <brk/types.h>

#define FNV1A_32_INIT 0x811C9DC5
#define FNV1A_32_PRIME 0x01000193

uint32_t fnv1a_32(const void *data, size_t len);
uint32_t hash_combine32(uint32_t a, uint32_t b);

#endif
