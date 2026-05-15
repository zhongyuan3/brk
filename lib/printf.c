#include <brk/errno.h>
#include <brk/kernel.h>
#include <brk/printf.h>
#include <brk/string.h>

#define ZERO_PAD (1U << 0)
#define LEFT_ALIGN (1U << 1)
#define MARK_POS (1U << 2)
#define ALT_FORM (1U << 3)
#define PAD_POS (1U << 4)

union fmt_arg {
	struct {
		uintmax_t i;
		bool lt0;
	};
	void *p;
};

struct internal_display {
	struct display *dis;
	usize_t cnt;
};

enum {
	STATE_INVALID,
	STATE_START,
	STATE_LPRE,
	STATE_LLPRE,
	STATE_HPRE,
	STATE_HHPRE,
	STATE_ZPRE,
	STATE_JPRE,
	STATE_TPRE,
	STATE_STOP,
	STATE_CHAR,
	STATE_UCHAR,
	STATE_SHORT,
	STATE_USHORT,
	STATE_INT,
	STATE_UINT,
	STATE_LONG,
	STATE_ULONG,
	STATE_LLONG,
	STATE_ULLONG,
	STATE_PTR,
	STATE_SIZE_T,
	STATE_PTRDIFF_T,
	STATE_INTMAX,
	STATE_UINTMAX,
	STATE_T_UINT,
	STATE_N_INT,
	STATE_N_CHAR,
	STATE_N_SHORT,
	STATE_N_LONG,
	STATE_N_LLONG,
	STATE_N_INTMAX,
	STATE_N_SIZE,
	STATE_N_PTRDIFF,
};

#define S(x) [x - 'A']
#define OOB(x) ((unsigned)(x) - 'A' > 'z' - 'A')

static u8 const states[STATE_STOP + 1]['z' - 'A' + 1] = {
	{ STATE_INVALID },
	{
		/* STATE_START */
		S('l') = STATE_LPRE,
		S('h') = STATE_HPRE,
		S('z') = STATE_ZPRE,
		S('j') = STATE_JPRE,
		S('t') = STATE_TPRE,
		S('d') = STATE_INT,
		S('i') = STATE_INT,
		S('u') = STATE_UINT,
		S('x') = STATE_UINT,
		S('X') = STATE_UINT,
		S('o') = STATE_UINT,
		S('s') = STATE_PTR,
		S('c') = STATE_INT,
		S('p') = STATE_PTR,
		S('n') = STATE_N_INT,
	},
	{
		/* STATE_LPRE */
		S('l') = STATE_LLPRE,
		S('d') = STATE_LONG,
		S('i') = STATE_LONG,
		S('u') = STATE_ULONG,
		S('x') = STATE_ULONG,
		S('X') = STATE_ULONG,
		S('o') = STATE_ULONG,
		S('n') = STATE_N_LONG,
	},
	{
		/* STATE_LLPRE */
		S('d') = STATE_LLONG,
		S('i') = STATE_LLONG,
		S('u') = STATE_ULLONG,
		S('x') = STATE_ULLONG,
		S('X') = STATE_ULLONG,
		S('o') = STATE_ULLONG,
		S('n') = STATE_N_LLONG,
	},
	{
		/* STATE_HPRE */
		S('l') = STATE_HHPRE,
		S('d') = STATE_SHORT,
		S('i') = STATE_SHORT,
		S('u') = STATE_USHORT,
		S('x') = STATE_USHORT,
		S('X') = STATE_USHORT,
		S('o') = STATE_USHORT,
		S('n') = STATE_N_SHORT,
	},
	{
		/* STATE_HHPRE */
		S('d') = STATE_CHAR,
		S('i') = STATE_CHAR,
		S('u') = STATE_UCHAR,
		S('x') = STATE_UCHAR,
		S('X') = STATE_UCHAR,
		S('o') = STATE_UCHAR,
		S('n') = STATE_N_CHAR,
	},
	{
		/* STATE_ZPRE */
		S('d') = STATE_PTRDIFF_T,
		S('i') = STATE_PTRDIFF_T,
		S('u') = STATE_SIZE_T,
		S('x') = STATE_SIZE_T,
		S('X') = STATE_SIZE_T,
		S('o') = STATE_SIZE_T,
		S('n') = STATE_N_SIZE,
	},
	{
		/* STATE_JPRE */
		S('d') = STATE_INTMAX,
		S('i') = STATE_INTMAX,
		S('u') = STATE_UINTMAX,
		S('x') = STATE_UINTMAX,
		S('X') = STATE_UINTMAX,
		S('o') = STATE_UINTMAX,
		S('n') = STATE_N_INTMAX,
	},
	{
		/* STATE_TPRE */
		S('d') = STATE_PTRDIFF_T,
		S('i') = STATE_PTRDIFF_T,
		S('u') = STATE_T_UINT,
		S('x') = STATE_T_UINT,
		S('X') = STATE_T_UINT,
		S('o') = STATE_T_UINT,
		S('n') = STATE_N_PTRDIFF,
	},
	{ STATE_INVALID },
};

static char *fmt_u(uintmax_t x, char *s, char const *d)
{
	do {
		*--s = d[x % 10];
		x /= 10;
	} while (x > 0);
	return s;
}

static char *fmt_o(uintmax_t x, char *s, char const *d)
{
	do {
		*--s = d[x & 7];
		x >>= 3;
	} while (x > 0);
	return s;
}

static char *fmt_x(uintmax_t x, char *s, char const *d)
{
	do {
		*--s = d[x & 15];
		x >>= 4;
	} while (x > 0);
	return s;
}

static void out(struct internal_display *dis, char const *buf, usize_t len)
{
	usize_t n = 0;
	dis->dis->write(dis->dis, buf, len, &n);
	dis->cnt += n;
}

static void pad(struct internal_display *dis, usize_t pad_len, char pad_ch)
{
	for (usize_t i = 0; i < pad_len; ++i)
		out(dis, &pad_ch, 1);
}

static void pop_n(va_list *ap, unsigned int st, usize_t cnt)
{
	switch (st) {
	case STATE_N_INT:
		*va_arg(*ap, int *) = (int)cnt;
		break;
	case STATE_N_CHAR:
		*va_arg(*ap, signed char *) = (signed char)cnt;
		break;
	case STATE_N_SHORT:
		*va_arg(*ap, short *) = (short)cnt;
		break;
	case STATE_N_LONG:
		*va_arg(*ap, long *) = (long)cnt;
		break;
	case STATE_N_LLONG:
		*va_arg(*ap, long long *) = (long long)cnt;
		break;
	case STATE_N_INTMAX:
		*va_arg(*ap, intmax_t *) = (intmax_t)cnt;
		break;
	case STATE_N_SIZE:
		*va_arg(*ap, usize_t *) = cnt;
		break;
	case STATE_N_PTRDIFF:
		*va_arg(*ap, ptrdiff_t *) = (ptrdiff_t)cnt;
		break;
	default:
		break;
	}
}

static void pop_arg(va_list *ap, unsigned int st, union fmt_arg *arg)
{
	signed char c;
	signed short si;
	signed int i;
	signed long li;
	signed long long lli;
	ptrdiff_t pd;
	intmax_t im;
	arg->lt0 = false;
	switch (st) {
	case STATE_CHAR:
		c = (signed char)va_arg(*ap, int);
		if (c < 0) {
			arg->lt0 = true;
			c = -c;
		}
		arg->i = c;
		break;
	case STATE_SHORT:
		si = (signed short)va_arg(*ap, int);
		if (si < 0) {
			arg->lt0 = true;
			si = -si;
		}
		arg->i = si;
		break;
	case STATE_INT:
		i = va_arg(*ap, int);
		if (i < 0) {
			arg->lt0 = true;
			i = -i;
		}
		arg->i = i;
		break;
	case STATE_LONG:
		li = va_arg(*ap, long);
		if (li < 0) {
			arg->lt0 = true;
			li = -li;
		}
		arg->i = li;
		break;
	case STATE_LLONG:
		lli = va_arg(*ap, long long);
		if (lli < 0) {
			arg->lt0 = true;
			lli = -lli;
		}
		arg->i = lli;
		break;
	case STATE_PTRDIFF_T:
		pd = va_arg(*ap, ptrdiff_t);
		if (pd < 0) {
			arg->lt0 = true;
			pd = -pd;
		}
		arg->i = pd;
		break;
	case STATE_UCHAR:
		arg->i = va_arg(*ap, unsigned int);
		break;
	case STATE_USHORT:
		arg->i = va_arg(*ap, unsigned int);
		break;
	case STATE_UINT:
		arg->i = va_arg(*ap, unsigned int);
		break;
	case STATE_ULONG:
		arg->i = va_arg(*ap, unsigned long);
		break;
	case STATE_ULLONG:
		arg->i = va_arg(*ap, unsigned long long);
		break;
	case STATE_SIZE_T:
		arg->i = va_arg(*ap, usize_t);
		break;
	case STATE_PTR:
		arg->p = va_arg(*ap, void *);
		break;
	case STATE_INTMAX:
		im = va_arg(*ap, intmax_t);
		if (im < 0) {
			arg->lt0 = true;
			arg->i = (uintmax_t)(-(uintmax_t)im);
		} else {
			arg->i = (uintmax_t)im;
		}
		break;
	case STATE_UINTMAX:
		arg->i = va_arg(*ap, uintmax_t);
		break;
	case STATE_T_UINT:
		pd = va_arg(*ap, ptrdiff_t);
		arg->i = (uintmax_t)pd;
		break;
	}
}

int printf_core(struct display *dis, char const *format, va_list ap)
{
	char buf[32];
	union fmt_arg arg = { 0 };
	struct internal_display idis = {
		.dis = dis,
		.cnt = 0,
	};
	char const *s = format;

	while (*s) {
		if (*s != '%') {
			out(&idis, s++, 1);
			continue;
		}

		++s;

		usize_t width = 0;
		bool has_width = false;
		usize_t prec = 0;
		bool has_prec = false;
		unsigned int flags = 0;

		while (1) {
			if (*s == ' ')
				flags |= PAD_POS;
			else if (*s == '+')
				flags |= MARK_POS;
			else if (*s == '-')
				flags |= LEFT_ALIGN;
			else if (*s == '0')
				flags |= ZERO_PAD;
			else if (*s == '#')
				flags |= ALT_FORM;
			else
				break;
			++s;
		}

		if (*s == '*') {
			++s;
			has_width = true;
			int fw = va_arg(ap, int);
			if (fw < 0) {
				flags |= LEFT_ALIGN;
				width = (usize_t)(-(unsigned)fw);
			} else {
				width = (usize_t)fw;
			}
		} else if (*s >= '0' && *s <= '9') {
			has_width = true;
			width = *s++ - '0';
			while (*s >= '0' && *s <= '9')
				width = width * 10 + (*s++ - '0');
		}

		if (*s == '.') {
			++s;
			if (*s == '*') {
				++s;
				int pr = va_arg(ap, int);
				if (pr >= 0) {
					has_prec = true;
					prec = (usize_t)pr;
				}
			} else {
				has_prec = true;
				while (*s >= '0' && *s <= '9')
					prec = prec * 10 + (*s++ - '0');
			}
		}

		if (*s == '%') {
			++s;
			char pct = '%';
			out(&idis, &pct, 1);
			continue;
		}

		unsigned int st = STATE_START;
		char pch = 0;
		while (st >= STATE_START && st <= STATE_STOP) {
			if (OOB(*s))
				goto invalid;
			st = states[st] S(*s);
			pch = *s++;
		}

		if (st == STATE_INVALID)
			goto invalid;

		if (st >= STATE_N_INT && st <= STATE_N_PTRDIFF) {
			pop_n(&ap, st, idis.cnt);
			continue;
		}

		pop_arg(&ap, st, &arg);

		char const *prefixes = "+- 0x0X";
		char const *digits = "0123456789abcdef";

		char *raw = NULL;
		usize_t raw_len = 0;

		char const *radix = NULL;
		usize_t radix_len = 0;
		char const *sign = NULL;
		usize_t sign_len = 0;

		usize_t lspace_pad = 0;
		usize_t lzero_pad = 0;
		usize_t rspace_pad = 0;

		switch (pch) {
		case 'i':
		case 'd':
		case 'u':
			flags &= ~ALT_FORM;
			raw = fmt_u(arg.i, buf + sizeof(buf), digits);
			break;
		case 'o':
			raw = fmt_o(arg.i, buf + sizeof(buf), digits);
			radix = prefixes + 3;
			if (flags & ALT_FORM) {
				radix_len = 1;
				if (prec > 0)
					prec -= 1;
			}
			break;
		case 'x':
			raw = fmt_x(arg.i, buf + sizeof(buf), digits);
			radix = prefixes + 3;
			if (flags & ALT_FORM)
				radix_len = 2;
			break;
		case 'X':
			digits = "0123456789ABCDEF";
			raw = fmt_x(arg.i, buf + sizeof(buf), digits);
			radix = prefixes + 5;
			if (flags & ALT_FORM)
				radix_len = 2;
			break;
		case 'p':
			flags = ALT_FORM;
			radix = prefixes + 3;
			radix_len = 2;
			raw = fmt_x(arg.i, buf + sizeof(buf), digits);
			break;
		case 's':
			if (arg.p) {
				raw_len = strlen(arg.p);
			} else {
				arg.p = "(null)";
				raw_len = 6;
			}
			if (has_prec && prec < raw_len)
				raw_len = prec;
			if (has_width && width > raw_len &&
			    !(flags & LEFT_ALIGN))
				pad(&idis, width - raw_len, ' ');
			out(&idis, arg.p, raw_len);
			if (has_width && width > raw_len &&
			    (flags & LEFT_ALIGN))
				pad(&idis, width - raw_len, ' ');
			continue;
		case 'c':
			pch = arg.i;
			out(&idis, &pch, 1);
			continue;
		}

		raw_len = buf + sizeof(buf) - raw;

		if (has_prec || (flags & LEFT_ALIGN))
			flags &= ~ZERO_PAD;

		if (has_prec && prec > raw_len)
			lzero_pad = prec - raw_len;

		if (flags & PAD_POS) {
			sign = prefixes + 2;
			sign_len = 1;
		}
		if (arg.lt0) {
			sign = prefixes + 1;
			sign_len = 1;
		}
		if (flags & MARK_POS) {
			sign = prefixes + 0;
			sign_len = 1;
		}

		usize_t total_len = sign_len + radix_len + lzero_pad + raw_len;
		if (has_width && width > total_len) {
			usize_t pad_extra = width - total_len;
			if (flags & LEFT_ALIGN)
				rspace_pad = pad_extra;
			else if (flags & ZERO_PAD)
				lzero_pad = pad_extra;
			else
				lspace_pad = pad_extra;
		}

		pad(&idis, lspace_pad, ' ');
		out(&idis, sign, sign_len);
		out(&idis, radix, radix_len);
		pad(&idis, lzero_pad, '0');
		out(&idis, raw, raw_len);
		pad(&idis, rspace_pad, ' ');
	}

	return (int)idis.cnt;

invalid:
	return -1;
}

int snprintf(char *buf, usize_t size, char const *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int ret = vsnprintf(buf, size, format, ap);
	va_end(ap);
	return ret;
}

struct string_display {
	char *buf;
	usize_t size;
	usize_t pos;
};

static int string_display_write(struct display *dis, char const *buf,
				usize_t len, usize_t *wlen)
{
	struct string_display *sd = dis->priv;
	usize_t n = min(len, sd->size - sd->pos);
	if (n > 0) {
		memcpy(sd->buf + sd->pos, buf, n);
		sd->pos += n;
	}

	if (wlen)
		*wlen = len;

	return 0;
}

int vsnprintf(char *buf, usize_t size, char const *format, va_list ap)
{
	struct string_display sd = {
		.buf = buf,
		.size = size,
		.pos = 0,
	};
	struct display dis = {
		.write = string_display_write,
		.priv = &sd,
	};
	return printf_core(&dis, format, ap);
}
