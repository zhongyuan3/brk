#ifndef AOSD_STRING_H
#define AOSD_STRING_H

#include <aosd/types.h>

void *memcpy(void *dst, void const *src, size_t n);
void *memmove(void *dst, void const *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memchr(void const *s, int c, size_t n);
void *memrchr(void const *s, int c, size_t n);
int memcmp(void const *s1, void const *s2, size_t n);

size_t strlen(char const *s);
size_t strnlen(char const *s, size_t n);
int strcmp(char const *s1, char const *s2);
int strncmp(char const *s1, char const *s2, size_t n);
char *strcpy(char *dst, char const *src);
char *strncpy(char *dst, char const *src, size_t n);
char *strchr(char const *s, int c);
char *strrchr(char const *s, int c);
char *strstr(char const *haystack, char const *needle);

#endif
