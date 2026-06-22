#include <brk/kernel.h>
#include <brk/string.h>
#include <uapi/brk/errno.h>

void *memcpy(void *dst, const void *src, usize_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	for (usize_t i = 0; i < n; ++i)
		d[i] = s[i];

	return dst;
}

void *memmove(void *dst, const void *src, usize_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if (d < s) {
		for (usize_t i = 0; i < n; ++i)
			d[i] = s[i];
	} else if (d > s) {
		for (usize_t i = n; i > 0; --i)
			d[i - 1] = s[i - 1];
	}

	return dst;
}

void *memset(void *s, int c, usize_t n)
{
	unsigned char *p = (unsigned char *)s;
	unsigned char uc = (unsigned char)c;

	for (usize_t i = 0; i < n; ++i)
		p[i] = uc;

	return s;
}

void *memchr(const void *s, int c, usize_t n)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned char uc = (unsigned char)c;

	for (usize_t i = 0; i < n; ++i)
		if (p[i] == uc)
			return (void *)(p + i);

	return NULL;
}

void *memrchr(const void *s, int c, usize_t n)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned char uc = (unsigned char)c;

	for (usize_t i = n; i > 0; --i)
		if (p[i - 1] == uc)
			return (void *)(p + i - 1);

	return NULL;
}

int memcmp(const void *s1, const void *s2, usize_t n)
{
	const unsigned char *p1 = (const unsigned char *)s1;
	const unsigned char *p2 = (const unsigned char *)s2;

	for (usize_t i = 0; i < n; ++i)
		if (p1[i] != p2[i])
			return (p1[i] > p2[i]) ? 1 : -1;

	return 0;
}

void memswap(void *s1, void *s2, usize_t size)
{
	unsigned char t;
	unsigned char *p1 = s1;
	unsigned char *p2 = s2;

	for (usize_t i = 0; i < size; ++i) {
		t = p1[i];
		p1[i] = p2[i];
		p2[i] = t;
	}
}

usize_t strlen(const char *s)
{
	const char *p = s;
	while (*p != '\0')
		++p;
	return (usize_t)(p - s);
}

usize_t strnlen(const char *s, usize_t n)
{
	const char *p = s;
	while (n > 0 && *p != '\0') {
		++p;
		--n;
	}
	return (usize_t)(p - s);
}

int strcmp(const char *s1, const char *s2)
{
	while (*s1 && (*s1 == *s2)) {
		++s1;
		++s2;
	}
	return (*(unsigned char *)s1 - *(unsigned char *)s2);
}

int strncmp(const char *s1, const char *s2, usize_t n)
{
	if (n == 0)
		return 0;

	while (n-- > 0 && *s1 && (*s1 == *s2)) {
		++s1;
		++s2;
	}

	if (n == (usize_t)-1)
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

char *strncpy(char *dst, const char *src, usize_t n)
{
	char *ret = dst;
	usize_t i;

	for (i = 0; i < n && src[i] != '\0'; ++i)
		dst[i] = src[i];

	for (; i < n; ++i)
		dst[i] = '\0';

	return ret;
}

usize_t strlcpy(char *dst, char const *src, usize_t size)
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
	return (usize_t)(dst - d) + strlen(src);
}

char *strcat(char *dst, char const *src)
{
	strcpy(dst + strlen(dst), src);
	return dst;
}

char *strncat(char *dst, char const *src, usize_t n)
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

usize_t strlcat(char *dst, char const *src, usize_t size)
{
	usize_t len = strnlen(dst, size);
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

	usize_t needle_len = strlen(needle);
	usize_t haystack_len = strlen(haystack);
	if (needle_len > haystack_len)
		return NULL;

	char const *p = haystack;
	char const *end = p + haystack_len - needle_len + 1;
	for (; p < end; ++p)
		if (memcmp(p, needle, needle_len) == 0)
			return (char *)p;

	return NULL;
}

static const char *errmsgs[] = {
	[0] = "Success",
	[EILSEQ] = "Illegal byte sequence",
	[EDOM] = "Domain error",
	[ERANGE] = "Result not representable",
	[ENOTTY] = "Not a tty",
	[EACCES] = "Permission denied",
	[EPERM] = "Operation not permitted",
	[ENOENT] = "No such file or directory",
	[ESRCH] = "No such process",
	[EEXIST] = "File exists",
	[EOVERFLOW] = "Value too large for data type",
	[ENOSPC] = "No space left on device",
	[ENOMEM] = "Out of memory",
	[EBUSY] = "Resource busy",
	[EINTR] = "Interrupted system call",
	[EAGAIN] = "Resource temporarily unavailable",
	[ESPIPE] = "Invalid seek",
	[EXDEV] = "Cross-device link",
	[EROFS] = "Read-only file system",
	[ENOTEMPTY] = "Directory not empty",
	[ECONNRESET] = "Connection reset by peer",
	[ETIMEDOUT] = "Operation timed out",
	[ECONNREFUSED] = "Connection refused",
	[EHOSTDOWN] = "Host is down",
	[EHOSTUNREACH] = "Host is unreachable",
	[EADDRINUSE] = "Address in use",
	[EPIPE] = "Broken pipe",
	[EIO] = "I/O error",
	[ENXIO] = "No such device or address",
	[ENOTBLK] = "Block device required",
	[ENODEV] = "No such device",
	[ENOTDIR] = "Not a directory",
	[EISDIR] = "Is a directory",
	[ETXTBSY] = "Text file busy",
	[ENOEXEC] = "Exec format error",
	[EINVAL] = "Invalid argument",
	[E2BIG] = "Argument list too long",
	[ELOOP] = "Symbolic link loop",
	[ENAMETOOLONG] = "Filename too long",
	[ENFILE] = "Too many open files in system",
	[EMFILE] = "No file descriptors available",
	[EBADF] = "Bad file descriptor",
	[ECHILD] = "No child process",
	[EFAULT] = "Bad address",
	[EFBIG] = "File too large",
	[EMLINK] = "Too many links",
	[ENOLCK] = "No locks available",
	[EDEADLK] = "Resource deadlock would occur",
	[ENOTRECOVERABLE] = "State not recoverable",
	[EOWNERDEAD] = "Previous owner died",
	[ECANCELED] = "Operation canceled",
	[ENOSYS] = "Function not implemented",
	[ENOMSG] = "No message of desired type",
	[EIDRM] = "Identifier removed",
	[ENOSTR] = "Device not a stream",
	[ENODATA] = "No data available",
	[ETIME] = "Device timeout",
	[ENOSR] = "Out of streams resources",
	[ENOLINK] = "Link has been severed",
	[EPROTO] = "Protocol error",
	[EBADMSG] = "Bad message",
	[EBADFD] = "File descriptor in bad state",
	[ENOTSOCK] = "Not a socket",
	[EDESTADDRREQ] = "Destination address required",
	[EMSGSIZE] = "Message too large",
	[EPROTOTYPE] = "Protocol wrong type for socket",
	[ENOPROTOOPT] = "Protocol not available",
	[EPROTONOSUPPORT] = "Protocol not supported",
	[ESOCKTNOSUPPORT] = "Socket type not supported",
	[EOPNOTSUPP] = "Operation not supported on transport endpoint",
	[EPFNOSUPPORT] = "Protocol family not supported",
	[EAFNOSUPPORT] = "Address family not supported by protocol",
	[EADDRNOTAVAIL] = "Address not available",
	[ENETDOWN] = "Network is down",
	[ENETUNREACH] = "Network unreachable",
	[ENETRESET] = "Connection reset by network",
	[ECONNABORTED] = "Connection aborted",
	[ENOBUFS] = "No buffer space available",
	[EISCONN] = "Socket is connected",
	[ENOTCONN] = "Socket not connected",
	[ESHUTDOWN] = "Cannot send after socket shutdown",
	[EALREADY] = "Operation already in progress",
	[EINPROGRESS] = "Operation in progress",
	[ESTALE] = "Stale file handle",
	[EUCLEAN] = "Data consistency error",
	[ENAVAIL] = "Resource not available",
	[EREMOTEIO] = "Remote I/O error",
	[EDQUOT] = "Quota exceeded",
	[ENOMEDIUM] = "No medium found",
	[EMEDIUMTYPE] = "Wrong medium type",
	[EMULTIHOP] = "Multihop attempted",
	[ENOKEY] = "Required key not available",
	[EKEYEXPIRED] = "Key has expired",
	[EKEYREVOKED] = "Key has been revoked",
	[EKEYREJECTED] = "Key was rejected by service",
};

const char *strerror(int errnum)
{
	if (errnum < 0)
		errnum = -errnum;

	if ((usize_t)errnum >= countof(errmsgs))
		return "Unknown error";
	return errmsgs[errnum];
}

usize_t strspn(const char *s, const char *accept)
{
	bool map[256] = { false };
	for (int i = 0; accept[i] != '\0'; ++i)
		map[(int)accept[i]] = true;
	usize_t cnt = 0;
	while (map[(int)s[cnt]])
		++cnt;
	return cnt;
}

usize_t strcspn(const char *s, const char *reject)
{
	bool map[256] = { false };
	for (int i = 0; reject[i] != '\0'; ++i)
		map[(int)reject[i]] = true;
	usize_t cnt = 0;
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
