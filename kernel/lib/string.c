#include <brk/kernel.h>
#include <brk/string.h>
#include <uapi/brk/errno.h>

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

const char *strerror(int errnum)
{
	if (errnum < 0)
		errnum = -errnum;

	switch (errnum) {
	case 0:
		return "Success";
	case EILSEQ:
		return "Illegal byte sequence";
	case EDOM:
		return "Domain error";
	case ERANGE:
		return "Result not representable";
	case ENOTTY:
		return "Not a tty";
	case EACCES:
		return "Permission denied";
	case EPERM:
		return "Operation not permitted";
	case ENOENT:
		return "No such file or directory";
	case ESRCH:
		return "No such process";
	case EEXIST:
		return "File exists";
	case EOVERFLOW:
		return "Value too large for data type";
	case ENOSPC:
		return "No space left on device";
	case ENOMEM:
		return "Out of memory";
	case EBUSY:
		return "Resource busy";
	case EINTR:
		return "Interrupted system call";
	case EAGAIN:
		return "Resource temporarily unavailable";
	case ESPIPE:
		return "Invalid seek";
	case EXDEV:
		return "Cross-device link";
	case EROFS:
		return "Read-only file system";
	case ENOTEMPTY:
		return "Directory not empty";
	case ECONNRESET:
		return "Connection reset by peer";
	case ETIMEDOUT:
		return "Operation timed out";
	case ECONNREFUSED:
		return "Connection refused";
	case EHOSTDOWN:
		return "Host is down";
	case EHOSTUNREACH:
		return "Host is unreachable";
	case EADDRINUSE:
		return "Address in use";
	case EPIPE:
		return "Broken pipe";
	case EIO:
		return "I/O error";
	case ENXIO:
		return "No such device or address";
	case ENOTBLK:
		return "Block device required";
	case ENODEV:
		return "No such device";
	case ENOTDIR:
		return "Not a directory";
	case EISDIR:
		return "Is a directory";
	case ETXTBSY:
		return "Text file busy";
	case ENOEXEC:
		return "Exec format error";
	case EINVAL:
		return "Invalid argument";
	case E2BIG:
		return "Argument list too long";
	case ELOOP:
		return "Symbolic link loop";
	case ENAMETOOLONG:
		return "Filename too long";
	case ENFILE:
		return "Too many open files in system";
	case EMFILE:
		return "No file descriptors available";
	case EBADF:
		return "Bad file descriptor";
	case ECHILD:
		return "No child process";
	case EFAULT:
		return "Bad address";
	case EFBIG:
		return "File too large";
	case EMLINK:
		return "Too many links";
	case ENOLCK:
		return "No locks available";
	case EDEADLK:
		return "Resource deadlock would occur";
	case ENOTRECOVERABLE:
		return "State not recoverable";
	case EOWNERDEAD:
		return "Previous owner died";
	case ECANCELED:
		return "Operation canceled";
	case ENOSYS:
		return "Function not implemented";
	case ENOMSG:
		return "No message of desired type";
	case EIDRM:
		return "Identifier removed";
	case ENOSTR:
		return "Device not a stream";
	case ENODATA:
		return "No data available";
	case ETIME:
		return "Device timeout";
	case ENOSR:
		return "Out of streams resources";
	case ENOLINK:
		return "Link has been severed";
	case EPROTO:
		return "Protocol error";
	case EBADMSG:
		return "Bad message";
	case EBADFD:
		return "File descriptor in bad state";
	case ENOTSOCK:
		return "Not a socket";
	case EDESTADDRREQ:
		return "Destination address required";
	case EMSGSIZE:
		return "Message too large";
	case EPROTOTYPE:
		return "Protocol wrong type for socket";
	case ENOPROTOOPT:
		return "Protocol not available";
	case EPROTONOSUPPORT:
		return "Protocol not supported";
	case ESOCKTNOSUPPORT:
		return "Socket type not supported";
	case EOPNOTSUPP:
		return "Operation not supported on transport endpoint";
	case EPFNOSUPPORT:
		return "Protocol family not supported";
	case EAFNOSUPPORT:
		return "Address family not supported by protocol";
	case EADDRNOTAVAIL:
		return "Address not available";
	case ENETDOWN:
		return "Network is down";
	case ENETUNREACH:
		return "Network unreachable";
	case ENETRESET:
		return "Connection reset by network";
	case ECONNABORTED:
		return "Connection aborted";
	case ENOBUFS:
		return "No buffer space available";
	case EISCONN:
		return "Socket is connected";
	case ENOTCONN:
		return "Socket not connected";
	case ESHUTDOWN:
		return "Cannot send after socket shutdown";
	case EALREADY:
		return "Operation already in progress";
	case EINPROGRESS:
		return "Operation in progress";
	case ESTALE:
		return "Stale file handle";
	case EUCLEAN:
		return "Data consistency error";
	case ENAVAIL:
		return "Resource not available";
	case EREMOTEIO:
		return "Remote I/O error";
	case EDQUOT:
		return "Quota exceeded";
	case ENOMEDIUM:
		return "No medium found";
	case EMEDIUMTYPE:
		return "Wrong medium type";
	case EMULTIHOP:
		return "Multihop attempted";
	case ENOKEY:
		return "Required key not available";
	case EKEYEXPIRED:
		return "Key has expired";
	case EKEYREVOKED:
		return "Key has been revoked";
	case EKEYREJECTED:
		return "Key was rejected by service";
	default:
		return "Unknown error";
	}
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
