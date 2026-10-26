#include <brk/console.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/timekeeper.h>
#include <uapi/errno.h>

#define LOG_BUF_LEN (32 * 1024)

#define LOG_RECORD_MARK 0xffffffffu

/* One record in the log ring buffer. 8-byte aligned in the buffer. */
struct log_record {
	u32 len; /* Text length; LOG_RECORD_MARK marks a wrap point */
	u8 level;
	u8 flags;
	u16 pad;
	u64 ts_nsec;
};

static char log_buf[LOG_BUF_LEN] __attribute__((aligned(8)));
static u32 log_next_off;
static u32 log_first_off;

static SPINLOCK_DEFINE(printk_lock);
static char printk_buf[1024];

static u32 log_record_used(u32 len)
{
	return sizeof(struct log_record) + round_up(len, 8);
}

static void log_store(u8 level, const char *text, u32 len)
{
	struct log_record *rec;
	u32 total;

	if (len > LOG_BUF_LEN - sizeof(struct log_record))
		len = LOG_BUF_LEN - sizeof(struct log_record);
	total = log_record_used(len);

	if (log_next_off + total > LOG_BUF_LEN) {
		/*
		 * Not enough contiguous tail space: drop whatever was left
		 * unflushed (keep the newest records) and restart at 0.
		 */
		log_next_off = 0;
		log_first_off = 0;
	}

	rec = (struct log_record *)&log_buf[log_next_off];
	rec->len = len;
	rec->level = level;
	rec->flags = 0;
	rec->ts_nsec = timekeeper_initialized() ? ktime_get_boot_ns() : 0;
	memcpy(&log_buf[log_next_off + sizeof(struct log_record)], text, len);
	log_next_off += total;
}

static void log_flush(void)
{
	u32 off = log_first_off;

	while (off != log_next_off) {
		struct log_record *rec;
		u32 used;

		if (off + sizeof(struct log_record) > LOG_BUF_LEN)
			break;
		rec = (struct log_record *)&log_buf[off];
		if (rec->len == LOG_RECORD_MARK) {
			off = 0;
			continue;
		}
		used = log_record_used(rec->len);
		if (off + used > LOG_BUF_LEN)
			break;
		console_write_record(rec->level, rec->ts_nsec,
				     &log_buf[off + sizeof(struct log_record)],
				     rec->len);
		off += used;
	}
	log_first_off = log_next_off;
}

void printk(char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
}

/*
 * Format the message, append it to the log ring buffer and, if any console
 * is active, drain the buffer so the record is printed. Caller must hold
 * printk_lock.
 */
static void vprintk_locked(int level, const char *fmt, va_list ap)
{
	int n = vsnprintf(printk_buf, sizeof(printk_buf), fmt, ap);

	if (n > 0)
		log_store(level, printk_buf,
			  strnlen(printk_buf, sizeof(printk_buf)));
	if (console_has_active())
		log_flush();
}

void vprintk(char const *fmt, va_list ap)
{
	spinlock_acquire(&printk_lock);
	vprintk_locked(LOG_LEVEL_DEFAULT, fmt, ap);
	spinlock_release(&printk_lock);
}

void _printk(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	spinlock_acquire(&printk_lock);
	vprintk_locked(level, fmt, ap);
	spinlock_release(&printk_lock);
	va_end(ap);
}

void printk_flush_pending(void)
{
	spinlock_acquire(&printk_lock);
	log_flush();
	spinlock_release(&printk_lock);
}

void printk_panic_text(const char *text, size_t len)
{
	if (spinlock_holding(&printk_lock)) {
		/* Panic fired inside printk(): we already own the lock. */
		log_store(LOG_LEVEL_ERROR, text, len);
		log_flush();
	} else if (spinlock_tryacquire(&printk_lock)) {
		log_store(LOG_LEVEL_ERROR, text, len);
		log_flush();
		spinlock_release(&printk_lock);
	} else {
		/* Another CPU holds the lock; best-effort raw output. */
		console_write_record_direct(LOG_LEVEL_ERROR,
					    ktime_get_boot_ns(), text, len);
	}
}
