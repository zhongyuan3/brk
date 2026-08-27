#include <brk/base/types.h>
#include <brk/lib/printf.h>
#include <brk/lib/string.h>
#include <limits.h>
#include <uapi/brk/errno.h>

#define FMT_F_ZERO_PAD (1U << 0)
#define FMT_F_LEFT_ALIGN (1U << 1)
#define FMT_F_MARK_POS (1U << 2)
#define FMT_F_ALT_FORM (1U << 3)
#define FMT_F_PAD_POS (1U << 4)

enum fmt_state {
	FMT_S_INVALID,
	FMT_S_START,
	FMT_S_LPRE,
	FMT_S_LLPRE,
	FMT_S_HPRE,
	FMT_S_HHPRE,
	FMT_S_ZPRE,
	FMT_S_STOP,
	FMT_S_CHAR,
	FMT_S_UCHAR,
	FMT_S_SHORT,
	FMT_S_USHORT,
	FMT_S_INT,
	FMT_S_UINT,
	FMT_S_LONG,
	FMT_S_ULONG,
	FMT_S_LLONG,
	FMT_S_ULLONG,
	FMT_S_SIZE_T,
	FMT_S_SSIZE_T,
	FMT_S_PTR,
	FMT_S_STR,
};

struct fmt_ctx {
	char *buf;
	size_t size;
	size_t pos;
	size_t cnt;
};

struct fmt_arg {
	union {
		uintmax_t ui;
		void *ptr;
		const char *str;
	};
	bool neg;
};

static enum fmt_state transition(enum fmt_state st, char c)
{
	switch (st) {
	case FMT_S_START:
		switch (c) {
		case 'l':
			return FMT_S_LPRE;
		case 'h':
			return FMT_S_HPRE;
		case 'z':
			return FMT_S_ZPRE;
		case 'd':
		case 'i':
			return FMT_S_INT;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			return FMT_S_UINT;
		case 's':
			return FMT_S_STR;
		case 'c':
			return FMT_S_INT;
		case 'p':
			return FMT_S_PTR;
		default:
			return FMT_S_INVALID;
		}
	case FMT_S_LPRE:
		switch (c) {
		case 'l':
			return FMT_S_LLPRE;
		case 'd':
		case 'i':
			return FMT_S_LONG;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			return FMT_S_ULONG;
		default:
			return FMT_S_INVALID;
		}
	case FMT_S_LLPRE:
		switch (c) {
		case 'd':
		case 'i':
			return FMT_S_LLONG;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			return FMT_S_ULLONG;
		default:
			return FMT_S_INVALID;
		}
	case FMT_S_HPRE:
		switch (c) {
		case 'h':
			return FMT_S_HHPRE;
		case 'd':
		case 'i':
			return FMT_S_SHORT;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			return FMT_S_USHORT;
		default:
			return FMT_S_INVALID;
		}
	case FMT_S_HHPRE:
		switch (c) {
		case 'd':
		case 'i':
			return FMT_S_CHAR;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			return FMT_S_UCHAR;
		default:
			return FMT_S_INVALID;
		}
	case FMT_S_ZPRE:
		switch (c) {
		case 'd':
		case 'i':
			return FMT_S_SSIZE_T;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			return FMT_S_SIZE_T;
		default:
			return FMT_S_INVALID;
		}
	default:
		return FMT_S_INVALID;
	}
}

static char *fmt_u(uintmax_t x, char *s, const char *d)
{
	do {
		*--s = d[x % 10];
		x /= 10;
	} while (x > 0);
	return s;
}

static char *fmt_o(uintmax_t x, char *s, const char *d)
{
	do {
		*--s = d[x & 7];
		x >>= 3;
	} while (x > 0);
	return s;
}

static char *fmt_x(uintmax_t x, char *s, const char *d)
{
	do {
		*--s = d[x & 15];
		x >>= 4;
	} while (x > 0);
	return s;
}

static void out(struct fmt_ctx *ctx, const char *buf, size_t len)
{
	if (len == 0)
		return;

	size_t n = ctx->size - ctx->pos;
	if (n > 0) {
		if (n > len)
			n = len;
		memcpy(ctx->buf + ctx->pos, buf, n);
		ctx->pos += n;
	}
	ctx->cnt += len;
}

static void pad(struct fmt_ctx *ctx, size_t pad_len, char pad_ch)
{
	char pad_buf[32];

	memset(pad_buf, pad_ch, sizeof(pad_buf));
	for (size_t i = 0; i < pad_len; i += sizeof(pad_buf)) {
		size_t n = pad_len - i;
		if (n > sizeof(pad_buf))
			n = sizeof(pad_buf);
		out(ctx, pad_buf, n);
	}
}

static inline int vsnprintf_internal(struct fmt_ctx *ctx, const char *fmt,
				     va_list ap)
{
	char num_buf[32] = { 0 };
	struct fmt_arg arg = { 0 };
	const char *s = fmt;
	int err = 0;

	while (*s) {
		if (*s != '%') {
			out(ctx, s++, 1);
			continue;
		}

		++s;

		size_t width = 0;
		bool has_width = false;
		size_t prec = 0;
		bool has_prec = false;
		unsigned int flags = 0;

		while (1) {
			if (*s == ' ')
				flags |= FMT_F_PAD_POS;
			else if (*s == '+')
				flags |= FMT_F_MARK_POS;
			else if (*s == '-')
				flags |= FMT_F_LEFT_ALIGN;
			else if (*s == '0')
				flags |= FMT_F_ZERO_PAD;
			else if (*s == '#')
				flags |= FMT_F_ALT_FORM;
			else
				break;
			++s;
		}

		if (*s == '*') {
			++s;
			int w = va_arg(ap, int);
			if (w < 0) {
				flags |= FMT_F_LEFT_ALIGN;
				width = (size_t)(-(long long)w);
			} else {
				width = (size_t)w;
			}
			if (width > INT_MAX) {
				err = -EOVERFLOW;
				goto err;
			}
			has_width = true;
		} else if (*s >= '0' && *s <= '9') {
			has_width = true;
			width = *s++ - '0';
			while (*s >= '0' && *s <= '9') {
				unsigned int digit = *s - '0';
				if (width > (INT_MAX - digit) / 10) {
					err = -EOVERFLOW;
					goto err;
				}
				width = width * 10 + digit;
				++s;
			}
		}

		if (*s == '.') {
			++s;
			if (*s == '*') {
				++s;
				int p = va_arg(ap, int);
				if (p >= 0) {
					has_prec = true;
					prec = (size_t)p;
				}
				if (prec > INT_MAX) {
					err = -EOVERFLOW;
					goto err;
				}
			} else {
				has_prec = true;
				while (*s >= '0' && *s <= '9') {
					unsigned int digit = *s - '0';
					if (prec > (INT_MAX - digit) / 10) {
						err = -EOVERFLOW;
						goto err;
					}
					prec = prec * 10 + digit;
					++s;
				}
			}
		}

		if (*s == '%') {
			++s;
			char pct = '%';
			out(ctx, &pct, 1);
			continue;
		}

		enum fmt_state st = FMT_S_START;
		char pch = 0;
		while (st >= FMT_S_START && st <= FMT_S_STOP) {
			enum fmt_state next = transition(st, *s);
			if (next == FMT_S_INVALID) {
				err = -EINVAL;
				goto err;
			}
			pch = *s++;
			st = next;
		}

		if (st == FMT_S_INVALID) {
			err = -EINVAL;
			goto err;
		}

		intmax_t si = 0;
		arg.neg = false;

		switch (st) {
		case FMT_S_CHAR:
			si = (signed char)va_arg(ap, int);
			goto check_neg;
		case FMT_S_SHORT:
			si = (signed short)va_arg(ap, int);
			goto check_neg;
		case FMT_S_INT:
			si = va_arg(ap, int);
			goto check_neg;
		case FMT_S_LONG:
			si = va_arg(ap, long);
			goto check_neg;
		case FMT_S_LLONG:
			si = va_arg(ap, long long);
			goto check_neg;
		case FMT_S_SSIZE_T:
			si = va_arg(ap, ssize_t);
check_neg:
			if (si < 0) {
				arg.neg = true;
				si = -si;
			}
			arg.ui = si;
			break;
		case FMT_S_UCHAR:
			arg.ui = (unsigned char)va_arg(ap, unsigned int);
			break;
		case FMT_S_USHORT:
			arg.ui = (unsigned short)va_arg(ap, unsigned int);
			break;
		case FMT_S_UINT:
			arg.ui = va_arg(ap, unsigned int);
			break;
		case FMT_S_ULONG:
			arg.ui = va_arg(ap, unsigned long);
			break;
		case FMT_S_ULLONG:
			arg.ui = va_arg(ap, unsigned long long);
			break;
		case FMT_S_SIZE_T:
			arg.ui = va_arg(ap, size_t);
			break;
		case FMT_S_PTR:
			arg.ptr = va_arg(ap, void *);
			arg.ui = (uintmax_t)(uintptr_t)arg.ptr;
			break;
		case FMT_S_STR:
			arg.ptr = va_arg(ap, char *);
			break;
		case FMT_S_INVALID:
		case FMT_S_START:
		case FMT_S_LPRE:
		case FMT_S_LLPRE:
		case FMT_S_HPRE:
		case FMT_S_HHPRE:
		case FMT_S_ZPRE:
		case FMT_S_STOP:
			break;
		}

		const char *prefixes = "+- 0x0X";
		const char *digits = "0123456789abcdef";

		char *raw = NULL;
		size_t raw_len = 0;

		const char *radix = NULL;
		size_t radix_len = 0;
		const char *sign = NULL;
		size_t sign_len = 0;

		size_t lspace_pad = 0;
		size_t lzero_pad = 0;
		size_t rspace_pad = 0;

		switch (pch) {
		case 'i':
		case 'd':
		case 'u':
			flags &= ~FMT_F_ALT_FORM;
			if (arg.ui == 0 && has_prec && prec == 0)
				raw = num_buf + sizeof(num_buf);
			else
				raw = fmt_u(arg.ui, num_buf + sizeof(num_buf),
					    digits);
			break;
		case 'o':
			radix = prefixes + 3;
			if (arg.ui == 0 && has_prec && prec == 0) {
				raw = num_buf + sizeof(num_buf);
				if (flags & FMT_F_ALT_FORM)
					radix_len = 1;
			} else {
				raw = fmt_o(arg.ui, num_buf + sizeof(num_buf),
					    digits);
				if ((flags & FMT_F_ALT_FORM) && arg.ui != 0) {
					radix_len = 1;
					if (prec > 0)
						prec -= 1;
				}
			}
			break;
		case 'x':
			if (arg.ui == 0 && has_prec && prec == 0)
				raw = num_buf + sizeof(num_buf);
			else
				raw = fmt_x(arg.ui, num_buf + sizeof(num_buf),
					    digits);
			radix = prefixes + 3;
			if ((flags & FMT_F_ALT_FORM) && arg.ui != 0)
				radix_len = 2;
			break;
		case 'X':
			digits = "0123456789ABCDEF";
			if (arg.ui == 0 && has_prec && prec == 0)
				raw = num_buf + sizeof(num_buf);
			else
				raw = fmt_x(arg.ui, num_buf + sizeof(num_buf),
					    digits);
			radix = prefixes + 5;
			if ((flags & FMT_F_ALT_FORM) && arg.ui != 0)
				radix_len = 2;
			break;
		case 'p':
			if (arg.ptr == NULL) {
				out(ctx, "(nil)", 5);
				continue;
			}
			flags = FMT_F_ALT_FORM;
			radix = prefixes + 3;
			radix_len = 2;
			if (arg.ui == 0 && has_prec && prec == 0)
				raw = num_buf + sizeof(num_buf);
			else
				raw = fmt_x(arg.ui, num_buf + sizeof(num_buf),
					    digits);
			break;
		case 's':
			if (arg.str) {
				raw_len = strlen(arg.str);
			} else {
				arg.str = "(null)";
				raw_len = 6;
			}

			if (has_prec && prec < raw_len)
				raw_len = prec;

			if (has_width && width > raw_len &&
			    !(flags & FMT_F_LEFT_ALIGN))
				pad(ctx, width - raw_len, ' ');

			out(ctx, arg.str, raw_len);

			if (has_width && width > raw_len &&
			    (flags & FMT_F_LEFT_ALIGN))
				pad(ctx, width - raw_len, ' ');

			continue;

		case 'c': {
			char ch = (unsigned char)arg.ui;
			size_t c_len = 1;
			if (has_width && width > c_len &&
			    !(flags & FMT_F_LEFT_ALIGN))
				pad(ctx, width - c_len, ' ');
			out(ctx, &ch, 1);
			if (has_width && width > c_len &&
			    (flags & FMT_F_LEFT_ALIGN))
				pad(ctx, width - c_len, ' ');
			continue;
		}
		}

		raw_len = num_buf + sizeof(num_buf) - raw;

		if (has_prec || (flags & FMT_F_LEFT_ALIGN))
			flags &= ~FMT_F_ZERO_PAD;

		if (has_prec && prec > raw_len)
			lzero_pad = prec - raw_len;

		if (arg.neg) {
			sign = prefixes + 1;
			sign_len = 1;
		} else if (flags & FMT_F_MARK_POS) {
			sign = prefixes + 0;
			sign_len = 1;
		} else if (flags & FMT_F_PAD_POS) {
			sign = prefixes + 2;
			sign_len = 1;
		}

		size_t total_len = sign_len + radix_len + lzero_pad + raw_len;
		if (has_width && width > total_len) {
			size_t pad_extra = width - total_len;
			if (flags & FMT_F_LEFT_ALIGN)
				rspace_pad = pad_extra;
			else if (flags & FMT_F_ZERO_PAD)
				lzero_pad = pad_extra;
			else
				lspace_pad = pad_extra;
		}

		pad(ctx, lspace_pad, ' ');
		out(ctx, sign, sign_len);
		out(ctx, radix, radix_len);
		pad(ctx, lzero_pad, '0');
		out(ctx, raw, raw_len);
		pad(ctx, rspace_pad, ' ');
	}

	if (ctx->size > 0) {
		size_t null_pos = (ctx->pos < ctx->size) ? ctx->pos :
							   ctx->size - 1;
		ctx->buf[null_pos] = '\0';
	}

	if (ctx->cnt > INT_MAX)
		return -EOVERFLOW;
	return (int)ctx->cnt;

err:
	return err;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct fmt_ctx ctx = {
		.buf = buf,
		.size = size,
		.pos = 0,
		.cnt = 0,
	};
	return vsnprintf_internal(&ctx, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int ret = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return ret;
}
