#ifndef BRK_FREESTANDING_STRING_H
#define BRK_FREESTANDING_STRING_H

#include <stddef.h>

/*
 * Freestanding C string subset.
 *
 * This layer exists for the compiler, libgcc and imported code (vendor/).
 * The mem* names must keep their standard spelling: GCC can emit calls to
 * memcpy/memmove/memset/memcmp for structure copies and initialisation, and
 * libgcc may reference them as well.
 *
 * Kernel-specific helpers (strlcpy, strlcat, memrchr, memswap) live in
 * <brk/string.h>.
 */

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memchr(void const *s, int c, size_t n);
int memcmp(void const *s1, void const *s2, size_t n);

size_t strlen(char const *s);
size_t strnlen(char const *s, size_t n);

int strcmp(char const *s1, char const *s2);
int strncmp(char const *s1, char const *s2, size_t n);

char *strcpy(char *dst, char const *src);
char *strncpy(char *dst, char const *src, size_t n);
char *strcat(char *dst, char const *src);
char *strncat(char *dst, char const *src, size_t n);

char *strchr(char const *s, int c);
char *strrchr(char const *s, int c);
char *strstr(char const *haystack, char const *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strtok(char *str, const char *delim);

const char *strerror(int errnum);

#endif
