#include <aosd/string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	for (size_t i = 0; i < n; ++i)
		d[i] = s[i];

	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if (d < s) {
		for (size_t i = 0; i < n; ++i)
			d[i] = s[i];
	} else if (d > s) {
		for (size_t i = n; i > 0; --i)
			d[i - 1] = s[i - 1];
	}

	return dst;
}

void *memset(void *s, int c, size_t n)
{
	unsigned char *p = (unsigned char *)s;
	unsigned char uc = (unsigned char)c;

	for (size_t i = 0; i < n; ++i)
		p[i] = uc;

	return s;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned char uc = (unsigned char)c;

	for (size_t i = 0; i < n; ++i)
		if (p[i] == uc)
			return (void *)(p + i);

	return NULL;
}

void *memrchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned char uc = (unsigned char)c;

	for (size_t i = n; i > 0; --i)
		if (p[i - 1] == uc)
			return (void *)(p + i - 1);

	return NULL;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
	const unsigned char *p1 = (const unsigned char *)s1;
	const unsigned char *p2 = (const unsigned char *)s2;

	for (size_t i = 0; i < n; ++i)
		if (p1[i] != p2[i])
			return (p1[i] > p2[i]) ? 1 : -1;

	return 0;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p != '\0')
		++p;
	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t n)
{
	const char *p = s;
	while (n > 0 && *p != '\0') {
		++p;
		--n;
	}
	return (size_t)(p - s);
}

int strcmp(const char *s1, const char *s2)
{
	while (*s1 && (*s1 == *s2)) {
		++s1;
		++s2;
	}
	return (*(unsigned char *)s1 - *(unsigned char *)s2);
}

int strncmp(const char *s1, const char *s2, size_t n)
{
	if (n == 0)
		return 0;

	while (n-- > 0 && *s1 && (*s1 == *s2)) {
		++s1;
		++s2;
	}

	if (n == (size_t)-1)
		return 0;

	return (*(unsigned char *)s1 - *(unsigned char *)s2);
}

char *strcpy(char *dst, const char *src)
{
	char *ret = dst;
	while ((*dst++ = *src++) != '\0')
		;
	return ret;
}

char *strncpy(char *dst, const char *src, size_t n)
{
	char *ret = dst;
	size_t i;

	for (i = 0; i < n && src[i] != '\0'; ++i)
		dst[i] = src[i];

	for (; i < n; ++i)
		dst[i] = '\0';

	return ret;
}

char *strchr(const char *s, int c)
{
	unsigned char uc = (unsigned char)c;
	while (*s != '\0') {
		if (*(unsigned char *)s == uc)
			return (char *)s;
		++s;
	}

	if (uc == '\0')
		return (char *)s;

	return NULL;
}

char *strrchr(const char *s, int c)
{
	unsigned char uc = (unsigned char)c;
	const char *last = NULL;

	do {
		if (*(unsigned char *)s == uc)
			last = s;
	} while (*s++ != '\0');

	return (char *)last;
}

char *strstr(char const *haystack, char const *needle)
{
	if (*needle == '\0')
		return (char *)haystack;

	size_t needle_len = strlen(needle);
	size_t haystack_len = strlen(haystack);
	if (needle_len > haystack_len)
		return NULL;

	char const *p = haystack;
	char const *end = p + haystack_len - needle_len + 1;
	for (; p < end; ++p)
		if (memcmp(p, needle, needle_len) == 0)
			return (char *)p;

	return NULL;
}
