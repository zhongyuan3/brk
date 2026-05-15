#ifndef BRK_STRING_H
#define BRK_STRING_H

#include <brk/types.h>

void *memcpy(void *dst, void const *src, usize_t n);
void *memmove(void *dst, void const *src, usize_t n);
void *memset(void *s, int c, usize_t n);
void *memchr(void const *s, int c, usize_t n);
void *memrchr(void const *s, int c, usize_t n);
int memcmp(void const *s1, void const *s2, usize_t n);
void memswap(void *s1, void *s2, usize_t size);

usize_t strlen(char const *s);
usize_t strnlen(char const *s, usize_t n);

int strcmp(char const *s1, char const *s2);
int strncmp(char const *s1, char const *s2, usize_t n);

char *strcpy(char *dst, char const *src);
char *strncpy(char *dst, char const *src, usize_t n);
usize_t strlcpy(char *dst, char const *src, usize_t size);
char *strcat(char *dst, char const *src);
char *strncat(char *dst, char const *src, usize_t n);
usize_t strlcat(char *dst, char const *src, usize_t size);

char *strchr(char const *s, int c);
char *strrchr(char const *s, int c);
char *strstr(char const *haystack, char const *needle);
usize_t strspn(const char *s, const char *accept);
usize_t strcspn(const char *s, const char *reject);
char *strtok(char *str, const char *delim);

const char *strerror(int errnum);

#endif
