#include <brk/string.h>
#include <stdbool.h>
#include <uapi/errno.h>

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

void memswap(void *s1, void *s2, size_t size)
{
	unsigned char t;
	unsigned char *p1 = s1;
	unsigned char *p2 = s2;

	for (size_t i = 0; i < size; ++i) {
		t = p1[i];
		p1[i] = p2[i];
		p2[i] = t;
	}
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

size_t strlcpy(char *dst, char const *src, size_t size)
{
	char *d = dst;

	if (!size--)
		goto finally;

	while (size && *src) {
		--size;
		*dst++ = *src++;
	}

	*dst = '\0';

finally:
	return (size_t)(dst - d) + strlen(src);
}

char *strcat(char *dst, char const *src)
{
	strcpy(dst + strlen(dst), src);
	return dst;
}

char *strncat(char *dst, char const *src, size_t n)
{
	char *d = dst;
	dst += strlen(dst);
	while (n && *src) {
		--n;
		*dst++ = *src++;
	}
	*dst++ = '\0';
	return d;
}

size_t strlcat(char *dst, char const *src, size_t size)
{
	size_t len = strnlen(dst, size);
	if (len == size)
		return size + strlen(src);
	return len + strlcpy(dst + len, src, size - len);
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

size_t strspn(const char *s, const char *accept)
{
	bool map[256] = { false };
	for (int i = 0; accept[i] != '\0'; ++i)
		map[(int)accept[i]] = true;
	size_t cnt = 0;
	while (map[(int)s[cnt]])
		++cnt;
	return cnt;
}

size_t strcspn(const char *s, const char *reject)
{
	bool map[256] = { false };
	for (int i = 0; reject[i] != '\0'; ++i)
		map[(int)reject[i]] = true;
	size_t cnt = 0;
	while (!map[(int)s[cnt]])
		++cnt;
	return cnt;
}

char *strtok(char *str, const char *delim)
{
	static char *saved_ptr = NULL;

	char *start;

	if (str) {
		saved_ptr = str;
	} else {
		if (!saved_ptr)
			return NULL;
	}

	start = saved_ptr;

	while (*start != '\0') {
		bool is_delim = false;
		const char *d = delim;
		while (*d != '\0') {
			if (*start == *d) {
				is_delim = true;
				break;
			}
			d++;
		}

		if (!is_delim)
			break;

		start++;
	}

	if (*start == '\0') {
		saved_ptr = NULL;
		return NULL;
	}

	char *end = start;
	while (*end != '\0') {
		bool is_delim = false;
		const char *d = delim;
		while (*d != '\0') {
			if (*end == *d) {
				is_delim = true;
				break;
			}
			d++;
		}

		if (is_delim)
			break;

		end++;
	}

	if (*end != '\0') {
		*end = '\0';
		saved_ptr = end + 1;
	} else {
		saved_ptr = NULL;
	}

	return start;
}
