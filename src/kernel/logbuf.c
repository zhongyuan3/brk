#include <brk/console.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/logbuf.h>
#include <brk/string.h>
#include <brk/timekeeper.h>

/*
 * Log ring buffer, modeled on Linux's log_buf. Every printk() record is
 * appended here (with its level and boot timestamp) and drained to the
 * registered consoles; before any console is active the records simply
 * accumulate, so boot messages appear once the UART driver registers.
 */

#define LOG_BUF_LEN (32 * 1024)

#define LOG_RECORD_MARK 0xffffffffu

static char log_buf[LOG_BUF_LEN] __attribute__((aligned(8)));
static u32 log_next_off;
static u32 log_first_off;

static u32 log_record_used(u32 len)
{
	return sizeof(struct log_record) + round_up(len, 8);
}

void log_store(u8 level, const char *text, u32 len)
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

void log_flush(void)
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