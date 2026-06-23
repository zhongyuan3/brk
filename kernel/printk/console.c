#include <brk/console.h>
#include <brk/device.h>
#include <brk/list.h>
#include <brk/spinlock.h>
#include <brk/tty.h>
#include <brk/uart.h>

static LIST_DEFINE(console_list);
static SPINLOCK_DEFINE(console_lock);

void console_register(struct console *console)
{
	spinlock_acquire(&console_lock);
	list_add_tail(&console->list, &console_list);
	spinlock_release(&console_lock);
}

void console_unregister(struct console *console)
{
	spinlock_acquire(&console_lock);
	list_del(&console->list);
	spinlock_release(&console_lock);
}

void console_write_all(const char *s, size_t n)
{
	struct console *con;

	spinlock_acquire(&console_lock);
	list_for_each_entry(con, &console_list, list) {
		if (!con->active)
			continue;
		for (size_t i = 0; i < n; i++) {
			con->put_char(con, s[i]);
		}
	}
	spinlock_release(&console_lock);
}
