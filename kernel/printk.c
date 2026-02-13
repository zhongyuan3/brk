#include <aosd/console.h>
#include <aosd/errno.h>
#include <aosd/macros.h>
#include <aosd/printf.h>
#include <aosd/printk.h>

static struct console *console_head;

void console_register(struct console *con)
{
	con->next = console_head;
	console_head = con;
}

int console_unregister(struct console *con)
{
	struct console **prev = &console_head;
	while (*prev) {
		if (*prev == con) {
			*prev = con->next;
			return 0;
		}
		prev = &(*prev)->next;
	}
	return -EINVAL;
}

static void write_all_console(char const *buf, size_t n)
{
	struct console *con = console_head;
	while (con) {
		con->write(con, buf, n, NULL);
		con = con->next;
	}
}

static int display_write(struct display *dis, char const *buf, size_t len,
			 size_t *wlen)
{
	write_all_console(buf, len);

	if (wlen)
		*wlen = len;

	return 0;
}

void printk(char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
}

void vprintk(char const *fmt, va_list ap)
{
	struct display dis = {
		.write = display_write,
		.priv = NULL,
	};
	printf_core(&dis, fmt, ap);
}
