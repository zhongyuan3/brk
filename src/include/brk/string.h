#ifndef BRK_STRING_H
#define BRK_STRING_H

#include <string.h>

void *memrchr(void const *s, int c, size_t n);
void memswap(void *s1, void *s2, size_t size);

size_t strlcpy(char *dst, char const *src, size_t size);
size_t strlcat(char *dst, char const *src, size_t size);

#endif
