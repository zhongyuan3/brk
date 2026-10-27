#include <brk/console.h>
#include <brk/list.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <uapi/errno.h>

static LIST_DEFINE(console_list);
static SPINLOCK_DEFINE(console_lock);

void console_register(struct console *console)
{
	irqstate_t flags = spin_lock_irqsave(&console_lock);

	list_add_tail(&console->list, &console_list);
	spin_unlock_irqrestore(&console_lock, flags);

	/* Print any messages logged before a console was available. */
	printk_flush_pending();
}

void console_unregister(struct console *console)
{
	irqstate_t flags = spin_lock_irqsave(&console_lock);

	list_del(&console->list);
	spin_unlock_irqrestore(&console_lock, flags);
}

bool console_has_active(void)
{
	struct console *con;
	bool active = false;
	irqstate_t flags = spin_lock_irqsave(&console_lock);

	list_for_each_entry(con, &console_list, list)
		if (con->active)
			active = true;
	spin_unlock_irqrestore(&console_lock, flags);
	return active;
}

static u8 console_log_level = KERN_DEBUG;

int console_set_log_level(u8 level)
{
	if (level > KERN_MAX)
		return -EINVAL;
	console_log_level = level;
	return 0;
}

static void console_write_record_locked(u8 level, u64 ts_nsec, const char *text,
					size_t len)
{
	struct console *con;
	char prefix[48];
	int pn;

	if (level > console_log_level)
		return;

	if (ts_nsec > 0) {
		pn = snprintf(prefix, sizeof(prefix),
			      "<%u>[%5" PRIu64 ".%06" PRIu64 "] ", level,
			      (u64)(ts_nsec / NS_PER_SEC),
			      (u64)((ts_nsec % NS_PER_SEC) / NS_PER_US));
	} else {
		pn = snprintf(prefix, sizeof(prefix), "<%u>[            ] ",
			      level);
	}
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

void console_write_record(u8 level, u64 ts_nsec, const char *text, size_t len)
{
	irqstate_t flags = spin_lock_irqsave(&console_lock);

	console_write_record_locked(level, ts_nsec, text, len);
	spin_unlock_irqrestore(&console_lock, flags);
}

void console_write_record_direct(u8 level, u64 ts_nsec, const char *text,
				 size_t len)
{
	console_write_record_locked(level, ts_nsec, text, len);
}