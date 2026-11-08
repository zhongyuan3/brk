#include <brk/align.h>
#include <brk/console.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/printk_log.h>
#include <brk/sched_clock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <uapi/errno.h>

static struct printk_ring_buffer printk_ring = {
	.lock = SPINLOCK_INITIALIZER("printk_ring"),
};

static char printk_buf[PRINTK_MAX_PAYLOAD];

/* Early boot needs the console immediately; the console thread turns this off. */
static bool printk_console_sync = true;

_Static_assert(sizeof(struct printk_log_header) == 16,
	       "printk_log_header must stay 16 bytes");
_Static_assert(offsetof(struct printk_log_header, len) == 4,
	       "printk_log_header layout changed");
_Static_assert(offsetof(struct printk_log_header, nsec) == 8,
	       "printk_log_header layout changed");
_Static_assert(PRINTK_RING_BUFFER_SIZE % sizeof(struct printk_log_header) == 0,
	       "ring size must be header-aligned");

static inline u32 printk_ring_off(u64 seq)
{
	return (u32)(seq & PRINTK_RING_BUFFER_MASK);
}

static inline u32 printk_record_total(u32 len)
{
	return (u32)align_up(sizeof(struct printk_log_header) + len,
			     alignof(struct printk_log_header));
}

static u64 printk_now_nsec(void)
{
	/*
	 * Use the boot clock, not CLOCK_MONOTONIC.  printk is available before
	 * timekeeper_init() establishes the monotonic epoch, so binding log
	 * timestamps to it would make them jump backwards during boot.
	 */
	return local_clock();
}

static bool printk_header_valid(const struct printk_log_header *h)
{
	if (h->magic[0] != PRINTK_LOG_MAGIC0 ||
	    h->magic[1] != PRINTK_LOG_MAGIC1)
		return false;

	if (h->flags & PRINTK_FLAGS_WRAP)
		return h->len <= PRINTK_RING_BUFFER_SIZE -
					 sizeof(struct printk_log_header);
	return h->len <= PRINTK_MAX_PAYLOAD;
}

/*
 * Size of the record at ring->tail.  When the header is corrupt, return all
 * currently used bytes so the caller drops everything instead of looping.
 */
static u32 printk_oldest_record_total(void)
{
	u32 off = printk_ring_off(printk_ring.tail);

	if (off + sizeof(struct printk_log_header) > PRINTK_RING_BUFFER_SIZE)
		return PRINTK_RING_BUFFER_SIZE - off;

	const struct printk_log_header *h =
		(const struct printk_log_header *)&printk_ring.buf[off];

	if (!printk_header_valid(h))
		return (u32)(printk_ring.head - printk_ring.tail);

	u32 total = printk_record_total(h->len);
	if (total == 0 || total > PRINTK_RING_BUFFER_SIZE ||
	    off + total > PRINTK_RING_BUFFER_SIZE)
		return (u32)(printk_ring.head - printk_ring.tail);
	return total;
}

static void printk_drop_oldest(void)
{
	if (printk_ring.tail == printk_ring.head)
		return;

	u32 step = printk_oldest_record_total();

	if (step == 0) {
		printk_ring.tail = printk_ring.head;
		return;
	}

	printk_ring.tail += step;
	if (printk_ring.tail > printk_ring.head)
		printk_ring.tail = printk_ring.head;
}

/*
 * Tail space is too small for a full record.  Write a WRAP record when there
 * is room for its header; otherwise zero-fill the tiny remainder.  In both
 * cases the reader can skip to the next ring boundary.
 */
static void printk_write_wrap(u32 off, u32 pad)
{
	if (pad >= sizeof(struct printk_log_header)) {
		struct printk_log_header *h =
			(struct printk_log_header *)&printk_ring.buf[off];

		h->magic[0] = PRINTK_LOG_MAGIC0;
		h->magic[1] = PRINTK_LOG_MAGIC1;
		h->level = KERN_DEBUG;
		h->flags = PRINTK_FLAGS_WRAP;
		h->len = pad - sizeof(struct printk_log_header);
		h->nsec = 0;
	} else {
		memset(&printk_ring.buf[off], 0, pad);
	}

	printk_ring.head += pad;
}

/* Caller must hold printk_ring.lock. */
static void printk_log_store_locked(u8 level, const char *text, size_t len,
				    u8 flags, u64 nsec)
{
	if (level > KERN_MAX)
		level = KERN_MAX;
	if (len == 0)
		return;
	if (len > PRINTK_MAX_PAYLOAD) {
		len = PRINTK_MAX_PAYLOAD;
		flags |= PRINTK_FLAGS_TRUNCATED;
	}

	u32 total = printk_record_total((u32)len);
	if (total > PRINTK_RING_BUFFER_SIZE)
		return;

	u32 off = printk_ring_off(printk_ring.head);
	u32 pad = 0;

	if ((u64)off + total > PRINTK_RING_BUFFER_SIZE)
		pad = PRINTK_RING_BUFFER_SIZE - off;

	u64 need = (u64)pad + total;
	if (need > PRINTK_RING_BUFFER_SIZE)
		return;

	while (printk_ring.head - printk_ring.tail + need >
	       PRINTK_RING_BUFFER_SIZE)
		printk_drop_oldest();

	if (pad) {
		printk_write_wrap(off, pad);
		off = printk_ring_off(printk_ring.head);
	}

	struct printk_log_header *h =
		(struct printk_log_header *)&printk_ring.buf[off];

	h->magic[0] = PRINTK_LOG_MAGIC0;
	h->magic[1] = PRINTK_LOG_MAGIC1;
	h->level = level;
	h->flags = flags;
	h->len = (u32)len;
	h->nsec = nsec;

	if (len)
		memcpy(&printk_ring.buf[off + sizeof(*h)], text, len);

	u32 used = (u32)sizeof(*h) + (u32)len;
	if (used < total)
		memset(&printk_ring.buf[off + used], 0, total - used);

	printk_ring.head += total;
}

size_t printk_log_write(u8 level, const char *text, size_t len, u64 nsec)
{
	u8 flags = 0;

	if (len > PRINTK_MAX_PAYLOAD) {
		len = PRINTK_MAX_PAYLOAD;
		flags |= PRINTK_FLAGS_TRUNCATED;
	}

	irqstate_t lock = spin_lock_irqsave(&printk_ring.lock);
	printk_log_store_locked(level, text, len, flags, nsec);
	spin_unlock_irqrestore(&printk_ring.lock, lock);

	return len;
}

void printk_log_reader_init(struct printk_log_reader *reader,
			    enum printk_log_start start)
{
	irqstate_t flags = spin_lock_irqsave(&printk_ring.lock);

	reader->ring = &printk_ring;
	reader->cursor = start == PRINTK_LOG_START_NEWEST ? printk_ring.head :
							    printk_ring.tail;
	reader->pending_lost = 0;

	spin_unlock_irqrestore(&printk_ring.lock, flags);
}

static int printk_log_read_common(struct printk_log_reader *reader, char *buf,
				  size_t size, u8 *level, u64 *nsec, u8 *flags,
				  bool trylock)
{
	if (!reader || !reader->ring || !buf || size == 0)
		return -EINVAL;

	struct printk_ring_buffer *ring = reader->ring;
	irqstate_t lock;

	if (trylock) {
		if (!spin_trylock_irqsave(&ring->lock, &lock))
			return -EAGAIN;
	} else {
		lock = spin_lock_irqsave(&ring->lock);
	}

	if (reader->cursor < ring->tail) {
		reader->pending_lost += ring->tail - reader->cursor;
		reader->cursor = ring->tail;
	}

	for (;;) {
		if (reader->cursor == ring->head)
			break;

		u32 off = printk_ring_off(reader->cursor);

		/* Tiny tail padding: no header fits here. */
		if (off + sizeof(struct printk_log_header) >
		    PRINTK_RING_BUFFER_SIZE) {
			reader->cursor += PRINTK_RING_BUFFER_SIZE - off;
			continue;
		}

		struct printk_log_header *h =
			(struct printk_log_header *)&ring->buf[off];

		if (!printk_header_valid(h)) {
			reader->pending_lost += ring->head - reader->cursor;
			reader->cursor = ring->head;
			spin_unlock_irqrestore(&ring->lock, lock);
			return -EIO;
		}

		u32 total = printk_record_total(h->len);
		if (total == 0 || total > PRINTK_RING_BUFFER_SIZE ||
		    off + total > PRINTK_RING_BUFFER_SIZE ||
		    reader->cursor + total > ring->head) {
			reader->pending_lost += ring->head - reader->cursor;
			reader->cursor = ring->head;
			spin_unlock_irqrestore(&ring->lock, lock);
			return -EIO;
		}

		if (h->flags & PRINTK_FLAGS_WRAP) {
			reader->cursor += total;
			continue;
		}

		if ((size_t)h->len + 1 > size) {
			reader->cursor += total;
			spin_unlock_irqrestore(&ring->lock, lock);
			return -ENOSPC;
		}

		if (h->len)
			memcpy(buf, &ring->buf[off + sizeof(*h)], h->len);
		buf[h->len] = '\0';

		if (level)
			*level = h->level;
		if (nsec)
			*nsec = h->nsec;
		if (flags)
			*flags = h->flags;

		reader->cursor += total;
		spin_unlock_irqrestore(&ring->lock, lock);
		return (int)h->len;
	}

	spin_unlock_irqrestore(&ring->lock, lock);
	return 0;
}

int printk_log_read(struct printk_log_reader *reader, char *buf, size_t size,
		    u8 *level, u64 *nsec, u8 *flags)
{
	return printk_log_read_common(reader, buf, size, level, nsec, flags,
				      false);
}

int printk_log_read_try(struct printk_log_reader *reader, char *buf,
			size_t size, u8 *level, u64 *nsec, u8 *flags)
{
	return printk_log_read_common(reader, buf, size, level, nsec, flags,
				      true);
}

static void vprintk_common(int level, const char *fmt, va_list ap)
{
	u64 nsec = printk_now_nsec();
	irqstate_t lock = spin_lock_irqsave(&printk_ring.lock);
	int n = vsnprintf(printk_buf, sizeof(printk_buf), fmt, ap);

	if (n > 0) {
		size_t len = (size_t)n;
		u8 flags = 0;

		if (len >= sizeof(printk_buf)) {
			len = sizeof(printk_buf) - 1;
			flags |= PRINTK_FLAGS_TRUNCATED;
		}
		printk_log_store_locked(level, printk_buf, len, flags, nsec);
	}
	spin_unlock_irqrestore(&printk_ring.lock, lock);

	if (printk_console_sync)
		(void)printk_flush_pending();
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
	vprintk_common(KERN_DEFAULT, fmt, ap);
}

void _printk(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintk_common(level, fmt, ap);
	va_end(ap);
}

void printk_console_set_async(bool async)
{
	printk_console_sync = !async;
}

void printk_panic_text(const char *text, size_t len)
{
	u64 nsec = printk_now_nsec();

	/* Best-effort: drain records queued before the panic (trap dumps etc.). */
	console_flush_on_panic();

	/*
	 * Never wait for another CPU on the panic path.  If the ring lock is
	 * available, preserve the panic message in the ring as well.
	 */
	if (!spin_is_locked_by_me(&printk_ring.lock) &&
	    spin_trylock(&printk_ring.lock)) {
		printk_log_store_locked(KERN_EMERG, text, len, 0, nsec);
		spin_unlock(&printk_ring.lock);
	}

	console_write_record_panic(KERN_EMERG, nsec, text, len);
}
