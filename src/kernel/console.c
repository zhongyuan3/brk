#include <brk/console.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/printk.h>
#include <brk/printk_log.h>
#include <brk/printf.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <uapi/errno.h>

static LIST_DEFINE(console_list);
static SPINLOCK_DEFINE(console_lock);
static bool console_panic_mode;

void console_register(struct console *console, u8 log_level)
{
	if (log_level > KERN_MAX)
		log_level = KERN_MAX;

	printk_log_reader_init(&console->reader, PRINTK_LOG_START_OLDEST);

	irqstate_t flags = spin_lock_irqsave(&console_lock);

	console->log_level = log_level;
	list_add_tail(&console->list, &console_list);
	spin_unlock_irqrestore(&console_lock, flags);

	/* Print any messages logged before this console was available. */
	(void)printk_flush_pending();
}

void console_unregister(struct console *console)
{
	irqstate_t flags = spin_lock_irqsave(&console_lock);

	list_del(&console->list);
	spin_unlock_irqrestore(&console_lock, flags);
}

void console_deactivate(struct console *console)
{
	irqstate_t flags = spin_lock_irqsave(&console_lock);

	console->active = false;
	spin_unlock_irqrestore(&console_lock, flags);
}

void console_set_boot_consoles_level(u8 level)
{
	struct console *con;

	if (level > KERN_MAX)
		level = KERN_MAX;

	irqstate_t flags = spin_lock_irqsave(&console_lock);

	list_for_each_entry(con, &console_list, list)
		if (con->flags & CONSOLE_FLAG_BOOT)
			con->log_level = level;

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

int console_set_log_level(struct console *console, u8 level)
{
	if (!console || level > KERN_MAX)
		return -EINVAL;

	irqstate_t flags = spin_lock_irqsave(&console_lock);

	console->log_level = level;
	spin_unlock_irqrestore(&console_lock, flags);
	return 0;
}

static int console_format_prefix(char *prefix, size_t size, u8 level,
				 u64 ts_nsec)
{
	if (ts_nsec > 0)
		return snprintf(prefix, size,
				"<%u>[%5" PRIu64 ".%06" PRIu64 "] ", level,
				(u64)(ts_nsec / NS_PER_SEC),
				(u64)((ts_nsec % NS_PER_SEC) / NS_PER_US));
	return snprintf(prefix, size, "<%u>[            ] ", level);
}

static bool console_write_one_locked(struct console *con, u8 level, u64 ts_nsec,
				     const char *text, size_t len, u8 flags)
{
	char prefix[48];
	int pn;

	if (!con->active || level > con->log_level)
		return false;

	pn = console_format_prefix(prefix, sizeof(prefix), level, ts_nsec);
	if (pn < 0)
		return false;
	if ((size_t)pn >= sizeof(prefix))
		pn = sizeof(prefix) - 1;

	for (int i = 0; i < pn; i++)
		con->put_char(con, prefix[i]);
	for (size_t i = 0; i < len; i++)
		con->put_char(con, text[i]);
	if (flags & PRINTK_FLAGS_TRUNCATED) {
		con->put_char(con, '.');
		con->put_char(con, '.');
		con->put_char(con, '.');
	}
	return true;
}

static int console_put_char_panic(struct console *con, int c)
{
	if (con->put_char_atomic)
		return con->put_char_atomic(con, c);
	return con->put_char(con, c);
}

static bool console_write_one_panic(struct console *con, u8 level, u64 ts_nsec,
				    const char *text, size_t len, u8 flags)
{
	char prefix[48];
	int pn;

	if (!con->active || level > con->log_level)
		return false;

	pn = console_format_prefix(prefix, sizeof(prefix), level, ts_nsec);
	if (pn < 0)
		return false;
	if ((size_t)pn >= sizeof(prefix))
		pn = sizeof(prefix) - 1;

	for (int i = 0; i < pn; i++)
		console_put_char_panic(con, prefix[i]);
	for (size_t i = 0; i < len; i++)
		console_put_char_panic(con, text[i]);
	if (flags & PRINTK_FLAGS_TRUNCATED) {
		console_put_char_panic(con, '.');
		console_put_char_panic(con, '.');
		console_put_char_panic(con, '.');
	}
	return true;
}

void console_write_record_panic(u8 level, u64 ts_nsec, const char *text,
				size_t len)
{
	struct console *con;

	list_for_each_entry(con, &console_list, list)
		console_write_one_panic(con, level, ts_nsec, text, len, 0);
}

void console_flush_on_panic(void)
{
	struct console *con;
	char rec[PRINTK_MAX_PAYLOAD + 1];

	console_panic_mode = true;

	list_for_each_entry(con, &console_list, list) {
		if (!con->active)
			continue;

		for (;;) {
			u8 level = 0;
			u8 flags = 0;
			u64 nsec = 0;
			int n;
			int tries = 0;

			do {
				n = printk_log_read_try(
					&con->reader, rec, sizeof(rec), &level,
					&nsec, &flags);
			} while (n == -EAGAIN && ++tries < 10000);

			if (con->reader.pending_lost) {
				char lostbuf[80];
				u64 lost = con->reader.pending_lost;
				int m;

				con->reader.pending_lost = 0;
				m = snprintf(lostbuf, sizeof(lostbuf),
					     "** %" PRIu64 " bytes lost **\n",
					     lost);
				if (m > 0) {
					size_t len = (size_t)m;

					if (len >= sizeof(lostbuf))
						len = sizeof(lostbuf) - 1;
					console_write_one_panic(con, KERN_EMERG,
								0, lostbuf,
								len, 0);
				}
			}

			if (n == 0 || n == -EAGAIN || n == -EIO)
				break;
			if (n == -ENOSPC)
				continue;
			if (n < 0)
				break;

			console_write_one_panic(con, level, nsec, rec,
						(size_t)n, flags);
		}
	}
}

static void console_write_record_locked(u8 level, u64 ts_nsec,
					const char *text, size_t len)
{
	struct console *con;

	list_for_each_entry(con, &console_list, list)
		console_write_one_locked(con, level, ts_nsec, text, len, 0);
}

void console_write_record(u8 level, u64 ts_nsec, const char *text, size_t len)
{
	irqstate_t flags = spin_lock_irqsave(&console_lock);

	console_write_record_locked(level, ts_nsec, text, len);
	spin_unlock_irqrestore(&console_lock, flags);
}

static bool console_flush_one_locked(struct console *con)
{
	char rec[PRINTK_MAX_PAYLOAD + 1];
	bool progress = false;

	if (console_panic_mode)
		return false;

	for (;;) {
		if (console_panic_mode)
			break;

		u8 level = 0;
		u8 flags = 0;
		u64 nsec = 0;
		int n = printk_log_read(&con->reader, rec, sizeof(rec), &level,
					&nsec, &flags);

		if (con->reader.pending_lost) {
			char lostbuf[80];
			u64 lost = con->reader.pending_lost;
			int m;

			con->reader.pending_lost = 0;
			m = snprintf(lostbuf, sizeof(lostbuf),
				     "** %" PRIu64 " bytes lost **\n", lost);
			if (m > 0) {
				size_t len = (size_t)m;

				if (len >= sizeof(lostbuf))
					len = sizeof(lostbuf) - 1;
				console_write_one_locked(con, KERN_WARN, 0,
							 lostbuf, len, 0);
			}
			progress = true;
		}

		if (n == 0)
			break;
		if (n == -ENOSPC) {
			progress = true;
			continue;
		}
		if (n < 0) {
			progress = true;
			break;
		}

		console_write_one_locked(con, level, nsec, rec, (size_t)n,
					 flags);
		progress = true;
	}

	return progress;
}

bool printk_flush_pending(void)
{
	struct console *con;
	bool progress = false;
	irqstate_t flags;

	/*
	 * trylock keeps a console driver that accidentally calls printk()
	 * from deadlocking the synchronous boot path.  The async thread will
	 * retry later.
	 */
	if (!spin_trylock_irqsave(&console_lock, &flags))
		return false;

	list_for_each_entry(con, &console_list, list) {
		if (!con->active)
			continue;
		progress |= console_flush_one_locked(con);
	}

	spin_unlock_irqrestore(&console_lock, flags);
	return progress;
}

static void console_log_thread(void *arg)
{
	(void)arg;

	for (;;) {
		if (!printk_flush_pending())
			ktime_nanosleep((ktime_t)10 * (ktime_t)NS_PER_MS,
					NULL);
	}
}

int console_log_start(void)
{
	static bool started;
	struct task *task;

	if (started)
		return 0;

	printk_console_set_async(true);
	task = kthread_create(console_log_thread, NULL);
	if (!task) {
		printk_console_set_async(false);
		return -ENOMEM;
	}

	strlcpy(task->name, "klog-console", sizeof(task->name));
	started = true;
	kthread_join(task);
	return 0;
}
