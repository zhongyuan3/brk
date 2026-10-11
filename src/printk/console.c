#include <brk/console.h>
#include <brk/list.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/timeconst.h>

static LIST_DEFINE(console_list);
static SPINLOCK_DEFINE(console_lock);

void console_register(struct console *console)
{
	spinlock_acquire(&console_lock);
	list_add_tail(&console->list, &console_list);
	spinlock_release(&console_lock);

	/* Print any messages logged before a console was available. */
	printk_flush_pending();
}

void console_unregister(struct console *console)
{
	spinlock_acquire(&console_lock);
	list_del(&console->list);
	spinlock_release(&console_lock);
}

bool console_has_active(void)
{
	struct console *con;
	bool active = false;

	spinlock_acquire(&console_lock);
	list_for_each_entry(con, &console_list, list)
		if (con->active)
			active = true;
	spinlock_release(&console_lock);
	return active;
}

static const char *const console_level_strs[] = {
	[KLOG_DEBUG] = "DEBUG",
	[KLOG_INFO] = "INFO",
	[KLOG_WARN] = "WARN",
	[KLOG_ERROR] = "ERR",
};

static void console_write_record_locked(uint8_t level, uint64_t ts_nsec,
					const char *text, size_t len)
{
	struct console *con;
	const char *lvl;
	char prefix[48];
	int pn;

	lvl = level <= KLOG_ERROR ? console_level_strs[level] : "?";
	pn = snprintf(prefix, sizeof(prefix), "[%5lu.%06lu] [%5s] ",
		      (unsigned long)(ts_nsec / NS_PER_SEC),
		      (unsigned long)((ts_nsec % NS_PER_SEC) / NS_PER_US), lvl);
	if (pn < 0)
		return;
	if ((size_t)pn > sizeof(prefix))
		pn = sizeof(prefix);

	list_for_each_entry(con, &console_list, list) {
		if (!con->active)
			continue;
		for (int i = 0; i < pn; i++)
			con->put_char(con, prefix[i]);
		for (size_t i = 0; i < len; i++)
			con->put_char(con, text[i]);
	}
}

void console_write_record(uint8_t level, uint64_t ts_nsec, const char *text,
			  size_t len)
{
	spinlock_acquire(&console_lock);
	console_write_record_locked(level, ts_nsec, text, len);
	spinlock_release(&console_lock);
}

void console_write_record_direct(uint8_t level, uint64_t ts_nsec,
				 const char *text, size_t len)
{
	console_write_record_locked(level, ts_nsec, text, len);
}